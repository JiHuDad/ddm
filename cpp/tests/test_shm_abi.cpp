// test_shm_abi.cpp — shm ABI invariants + arena create/attach + reject paths.
#include "test_framework.h"

#include "driftmon/shm_abi.h"
#include "shm_arena.h"

using namespace driftmon;

TEST(abi_golden_layout) {
    // Golden sizes — must match the static_asserts in shm_abi.h. A drift here
    // means the binary contract changed and SHM_VERSION must bump.
    CHECK(sizeof(ArenaHeader) == 24);
    CHECK(sizeof(SlotHeader) == 1216);
    CHECK(offsetof(SlotHeader, seq) % DRIFTMON_CACHELINE == 0);
    CHECK(std::atomic<uint64_t>::is_always_lock_free);
    CHECK(std::atomic<uint32_t>::is_always_lock_free);
    CHECK(DRIFTMON_SHM_VERSION == 2);      // v2: NaN bin + sample ring
    CHECK(DRIFTMON_EXTRA_BINS == 3);       // underflow + overflow + NaN
}

TEST(abi_layout_math) {
    // Two buffers, each cache-line aligned; arena holds header + one slot.
    CHECK(slot_header_padded_size() % DRIFTMON_CACHELINE == 0);
    CHECK(bins_stride(10) * sizeof(uint64_t) % DRIFTMON_CACHELINE == 0);
    CHECK(slot_bytes(10) > slot_header_padded_size());
    CHECK(arena_bytes(10) > slot_bytes(10));
}

static SlotSpec make_spec() {
    SlotSpec s;
    s.model_id = "abi_test";
    s.n_features = 2;
    s.bin_offset = {0, 5, 12};   // feature0 = 5 phys bins, feature1 = 7; total 12
    s.n_bins_total = 12;
    return s;
}

TEST(arena_create_attach_roundtrip) {
    Arena owner;
    std::string err;
    CHECK(arena_create(owner, "/driftmon.test_abi", make_spec(), err));
    CHECK(owner.valid());
    CHECK(owner.header()->magic == DRIFTMON_SHM_MAGIC);
    CHECK(owner.slot(0)->n_bins_total == 12);
    CHECK(std::string(owner.slot(0)->model_id) == "abi_test");

    Arena client;
    CHECK(arena_attach(client, "/driftmon.test_abi", 12, err));  // matching layout
    CHECK(client.valid());
    CHECK(client.slot(0)->n_features == 2);

    arena_detach(client);
    arena_detach(owner);
    arena_unlink("/driftmon.test_abi");
}

TEST(arena_attach_rejects) {
    Arena owner;
    std::string err;
    CHECK(arena_create(owner, "/driftmon.test_rej", make_spec(), err));

    Arena client;
    // n_bins_total mismatch ⇒ refuse (tap's bundle disagrees with worker layout).
    CHECK(!arena_attach(client, "/driftmon.test_rej", 99, err));
    // Nonexistent name ⇒ refuse.
    CHECK(!arena_attach(client, "/driftmon.does_not_exist_xyz", 12, err));

    // Corrupt magic ⇒ refuse.
    owner.header()->magic = 0;
    CHECK(!arena_attach(client, "/driftmon.test_rej", 12, err));

    arena_detach(owner);
    arena_unlink("/driftmon.test_rej");
}

int main() { return RUN_ALL(); }
