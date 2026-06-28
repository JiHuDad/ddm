// affinity.cpp — CPU pinning via sched_setaffinity (Linux/POSIX).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "affinity.h"

#include <sched.h>

#include <cerrno>
#include <cstring>
#include <sstream>

namespace driftmon {

bool parse_cpu_list(const std::string& spec, std::vector<int>& out) {
    out.clear();
    std::istringstream ss(spec);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        try {
            size_t pos = 0;
            int v = std::stoi(tok, &pos);
            if (pos != tok.size() || v < 0) return false;
            out.push_back(v);
        } catch (...) {
            return false;
        }
    }
    return !out.empty();
}

bool pin_to_cpus(const std::vector<int>& cpus, std::string& err) {
    if (cpus.empty()) return true;   // no-op
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cpus) CPU_SET(c, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        err = std::strerror(errno);
        return false;
    }
    return true;
}

}  // namespace driftmon
