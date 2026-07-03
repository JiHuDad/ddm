// shm_arena.cpp — POSIX shm create/attach/detach.
//
// Lock-free atomics live directly in the mapped region. The memory is zero-
// filled by ftruncate, so the control atomics start at 0 (active_idx=0, seq=0,
// inflight=0, sample_count=0) — the accepted shm idiom for is_always_lock_free
// atomics, asserted in shm_abi.h. Standard library + POSIX only.
#include "shm_arena.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace driftmon {

namespace {
// shm names must start with '/' and contain no other '/'. Normalize.
std::string shm_name(const std::string& name) {
    return name.empty() || name[0] == '/' ? name : "/" + name;
}
}  // namespace

bool arena_create(Arena& a, const std::string& name, const SlotSpec& spec,
                  std::string& err) {
    if (spec.n_bins_total == 0 || spec.n_features == 0) {
        err = "invalid slot spec (zero features/bins)";
        return false;
    }
    if (spec.bin_offset.size() != spec.n_features + 1u) {
        err = "bin_offset length must be n_features + 1";
        return false;
    }
    const std::string nm = shm_name(name);
    const size_t bytes = arena_bytes(spec.n_bins_total);

    // Fresh region: unlink any stale one first so size/contents are clean.
    shm_unlink(nm.c_str());
    int fd = shm_open(nm.c_str(), O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) { err = "shm_open(create) failed: " + std::string(std::strerror(errno)); return false; }
    if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        err = "ftruncate failed: " + std::string(std::strerror(errno));
        close(fd); shm_unlink(nm.c_str());
        return false;
    }
    void* base = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        err = "mmap failed: " + std::string(std::strerror(errno));
        close(fd); shm_unlink(nm.c_str());
        return false;
    }

    auto* hdr = reinterpret_cast<ArenaHeader*>(base);
    hdr->magic = DRIFTMON_SHM_MAGIC;
    hdr->version = DRIFTMON_SHM_VERSION;
    hdr->slot_count = 1;
    hdr->slot_offset[0] = round_up(sizeof(ArenaHeader), DRIFTMON_CACHELINE);

    SlotHeader* s = slot_at(hdr, 0);
    std::memset(s->model_id, 0, sizeof(s->model_id));
    std::strncpy(s->model_id, spec.model_id.c_str(), sizeof(s->model_id) - 1);
    s->n_features = spec.n_features;
    s->n_bins_total = spec.n_bins_total;
    std::memset(s->bin_offset, 0, sizeof(s->bin_offset));
    for (size_t i = 0; i < spec.bin_offset.size(); ++i)
        s->bin_offset[i] = spec.bin_offset[i];
    s->seq.store(0, std::memory_order_relaxed);
    s->active_idx.store(0, std::memory_order_relaxed);
    for (uint32_t b = 0; b < DRIFTMON_NUM_BUFFERS; ++b) {
        s->inflight[b].store(0, std::memory_order_relaxed);
        s->sample_count[b].store(0, std::memory_order_relaxed);
    }
    // Counter buffers are already zero from ftruncate; publish all of the above.
    std::atomic_thread_fence(std::memory_order_release);

    a.fd = fd; a.base = base; a.bytes = bytes; a.name = nm; a.is_owner = true;
    return true;
}

bool arena_attach(Arena& a, const std::string& name,
                  uint32_t expected_n_bins_total, std::string& err) {
    const std::string nm = shm_name(name);
    int fd = shm_open(nm.c_str(), O_RDWR, 0600);
    if (fd < 0) { err = "shm_open(attach) failed: " + std::string(std::strerror(errno)); return false; }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(ArenaHeader))) {
        err = "arena too small / fstat failed";
        close(fd);
        return false;
    }
    const size_t bytes = static_cast<size_t>(st.st_size);
    void* base = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        err = "mmap(attach) failed: " + std::string(std::strerror(errno));
        close(fd);
        return false;
    }

    auto* hdr = reinterpret_cast<ArenaHeader*>(base);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (hdr->magic != DRIFTMON_SHM_MAGIC) { err = "magic mismatch"; goto fail; }
    if (hdr->version != DRIFTMON_SHM_VERSION) { err = "version mismatch"; goto fail; }
    if (hdr->slot_count < 1) { err = "no slots"; goto fail; }
    {
        SlotHeader* s = slot_at(hdr, 0);
        if (s->n_bins_total != expected_n_bins_total) {
            err = "n_bins_total mismatch (tap bundle disagrees with worker layout)";
            goto fail;
        }
        if (bytes < arena_bytes(s->n_bins_total)) { err = "arena smaller than slot layout"; goto fail; }
    }

    a.fd = fd; a.base = base; a.bytes = bytes; a.name = nm; a.is_owner = false;
    return true;

fail:
    munmap(base, bytes);
    close(fd);
    return false;
}

bool arena_open_or_create(Arena& a, const std::string& name, const SlotSpec& spec,
                          bool& reused, std::string& err) {
    reused = false;
    // Probe for an existing, layout-compatible arena (worker restart case).
    Arena probe;
    std::string perr;
    if (arena_attach(probe, name, spec.n_bins_total, perr)) {
        SlotHeader* s = probe.slot(0);
        const bool match = s->n_features == spec.n_features &&
                           std::string(s->model_id) == spec.model_id;
        if (match) {
            probe.is_owner = true;   // we take over ownership (keep tap data intact)
            a = probe;
            reused = true;
            return true;
        }
        arena_detach(probe);         // name collision / stale layout — recreate
    }
    return arena_create(a, name, spec, err);
}

void arena_detach(Arena& a) {
    if (a.base) munmap(a.base, a.bytes);
    if (a.fd >= 0) close(a.fd);
    a.base = nullptr; a.fd = -1; a.bytes = 0; a.is_owner = false;
}

void arena_unlink(const std::string& name) {
    shm_unlink(shm_name(name).c_str());
}

bool arena_is_stale(const Arena& a) {
    if (a.fd < 0) return false;
    struct stat st;
    if (fstat(a.fd, &st) != 0) return true;   // fd gone bad — treat as stale
    return st.st_nlink == 0;
}

}  // namespace driftmon
