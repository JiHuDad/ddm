/*
 * demo_driver.cpp — command-line drift detection driver.
 *
 * Usage:
 *   demo_driver <reference.json> <observations.csv>
 *
 * Reads <observations.csv> row by row (header = feature names), feeds each
 * row into driftmon, and prints one summary line per completed tumbling window:
 *
 *   WINDOW 1: max_psi=0.0123  severity=STABLE    psi=[0.0123 0.0045 0.0098]
 *   WINDOW 2: max_psi=0.3812  severity=SIGNIFICANT psi=[0.3812 0.2941 0.1503]
 *
 * Exits 0 on success, non-zero on bad input.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "driftmon.h"

static const char* severity_str(driftmon_severity_t s) {
    switch (s) {
        case DRIFTMON_STABLE:      return "STABLE";
        case DRIFTMON_WARNING:     return "WARNING";
        case DRIFTMON_SIGNIFICANT: return "SIGNIFICANT";
        default:                   return "UNKNOWN";
    }
}

static std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::istringstream ss(line);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        // trim carriage return
        if (!tok.empty() && tok.back() == '\r') tok.pop_back();
        fields.push_back(tok);
    }
    return fields;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <reference.json> <observations.csv>\n", argv[0]);
        return 1;
    }

    driftmon_t* m = driftmon_create(argv[1]);
    if (!m) {
        std::fprintf(stderr, "error: failed to load reference.json: %s\n", argv[1]);
        return 1;
    }

    int nf = driftmon_num_features(m);

    std::ifstream csv(argv[2]);
    if (!csv) {
        std::fprintf(stderr, "error: cannot open observations: %s\n", argv[2]);
        driftmon_destroy(m);
        return 1;
    }

    // Validate header (informational — column order must match reference features).
    std::string header_line;
    if (!std::getline(csv, header_line)) {
        std::fprintf(stderr, "error: %s is empty\n", argv[2]);
        driftmon_destroy(m);
        return 1;
    }

    std::vector<double> feats(nf);
    std::vector<double> psi(nf);
    int window_num = 0;
    std::string line;

    while (std::getline(csv, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto fields = split_csv_line(line);
        if (static_cast<int>(fields.size()) < nf) continue; // skip short rows

        bool valid = true;
        for (int i = 0; i < nf; ++i) {
            char* end = nullptr;
            feats[i] = std::strtod(fields[i].c_str(), &end);
            if (end == fields[i].c_str()) { valid = false; break; }
        }
        if (!valid) continue;

        driftmon_observe(m, feats.data(), nf);

        if (driftmon_ready(m)) {
            double max_psi = 0.0;
            driftmon_compute(m, psi.data(), &max_psi);
            ++window_num;

            std::printf("WINDOW %d: max_psi=%.4f  severity=%-11s  psi=[",
                        window_num, max_psi,
                        severity_str(driftmon_classify(max_psi)));
            for (int i = 0; i < nf; ++i) {
                if (i) std::printf(" ");
                std::printf("%.4f", psi[i]);
            }
            std::printf("]\n");

            driftmon_reset(m);
        }
    }

    driftmon_destroy(m);
    return 0;
}
