/*
 * affinity.h — worker CPU pinning (SPEC NFR3).
 *
 * Pin the worker to housekeeping cores so it cannot preempt serving threads on
 * isolated/SCHED_FIFO cores. The worker stays SCHED_OTHER (default); this only
 * sets CPU affinity. Best-effort: failure is reported, not fatal.
 */
#ifndef DRIFTMON_AFFINITY_H
#define DRIFTMON_AFFINITY_H

#include <string>
#include <vector>

namespace driftmon {

// Pin the calling process to `cpus`. Empty list ⇒ no-op (returns true).
bool pin_to_cpus(const std::vector<int>& cpus, std::string& err);

// Parse a comma-separated CPU list ("2,3,5") into ids. Returns false on garbage.
bool parse_cpu_list(const std::string& spec, std::vector<int>& out);

}  // namespace driftmon

#endif  // DRIFTMON_AFFINITY_H
