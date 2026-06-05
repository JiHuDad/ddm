// json_min.cpp — minimal recursive-descent JSON parser for reference.json.
//
// Parses exactly the schema in SPEC.md §3. Not a general JSON library.
// Standard library only.
#include "json_min.h"

#include <algorithm>
#include <cerrno>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace dm {

namespace {

struct Parser {
    const char* p;
    const char* end;

    void skip_ws() {
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
    }

    bool peek(char c) {
        skip_ws();
        return p < end && *p == c;
    }

    bool consume(char c) {
        if (!peek(c)) return false;
        ++p;
        return true;
    }

    bool parse_string(std::string& s) {
        if (!consume('"')) return false;
        s.clear();
        while (p < end && *p != '"') {
            if (*p == '\\') {
                ++p;
                if (p >= end) return false;
                switch (*p) {
                    case '"':  s += '"';  break;
                    case '\\': s += '\\'; break;
                    case '/':  s += '/';  break;
                    case 'n':  s += '\n'; break;
                    case 'r':  s += '\r'; break;
                    case 't':  s += '\t'; break;
                    case 'b':  s += '\b'; break;
                    case 'f':  s += '\f'; break;
                    case 'u': {
                        // \uXXXX — 4 hex digits follow; decode to UTF-8.
                        // p currently points at 'u'; hex digits are at p[1]..p[4].
                        if (p + 4 >= end) return false;
                        uint32_t cp = 0;
                        for (int i = 1; i <= 4; ++i) {
                            char c2 = p[i];
                            uint32_t nibble;
                            if      (c2 >= '0' && c2 <= '9') nibble = static_cast<uint32_t>(c2 - '0');
                            else if (c2 >= 'a' && c2 <= 'f') nibble = static_cast<uint32_t>(c2 - 'a' + 10);
                            else if (c2 >= 'A' && c2 <= 'F') nibble = static_cast<uint32_t>(c2 - 'A' + 10);
                            else return false;
                            cp = (cp << 4) | nibble;
                        }
                        p += 4;  // outer ++p will step past the 4th hex digit
                        if (cp < 0x80u) {
                            s += static_cast<char>(cp);
                        } else if (cp < 0x800u) {
                            s += static_cast<char>(0xC0u | (cp >> 6));
                            s += static_cast<char>(0x80u | (cp & 0x3Fu));
                        } else {
                            s += static_cast<char>(0xE0u | (cp >> 12));
                            s += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
                            s += static_cast<char>(0x80u | (cp & 0x3Fu));
                        }
                        break;
                    }
                    default: return false;
                }
            } else {
                s += *p;
            }
            ++p;
        }
        return consume('"');
    }

    bool parse_number(double& v) {
        skip_ws();
        char* nend = nullptr;
        errno = 0;
        v = std::strtod(p, &nend);
        if (nend == p || errno == ERANGE) return false;
        if (!std::isfinite(v)) return false;  // NaN / Inf not valid in reference.json
        p = nend;
        return true;
    }

    bool parse_int(int& v) {
        double d;
        if (!parse_number(d)) return false;
        if (d != std::floor(d)) return false;              // reject fractional (e.g. 1.5)
        if (d < static_cast<double>(INT_MIN) ||
            d > static_cast<double>(INT_MAX)) return false;
        v = static_cast<int>(d);
        return true;
    }

    // Skip an unknown value (string, number, array, object, or literal).
    bool skip_value() {
        skip_ws();
        if (p >= end) return false;
        if (*p == '"') {
            std::string s;
            return parse_string(s);
        }
        if (*p == '[') {
            ++p;
            while (!peek(']')) {
                if (!skip_value()) return false;
                if (!consume(',')) break;
            }
            return consume(']');
        }
        if (*p == '{') {
            ++p;
            while (!peek('}')) {
                std::string k;
                if (!parse_string(k)) return false;
                if (!consume(':')) return false;
                if (!skip_value()) return false;
                if (!consume(',')) break;
            }
            return consume('}');
        }
        // Number, true, false, null — scan until delimiter.
        const char* start = p;
        while (p < end && *p != ',' && *p != ']' && *p != '}' &&
               !std::isspace(static_cast<unsigned char>(*p))) ++p;
        return p > start;
    }

    bool parse_double_array(std::vector<double>& arr) {
        if (!consume('[')) return false;
        arr.clear();
        if (peek(']')) { ++p; return true; }
        do {
            double v;
            if (!parse_number(v)) return false;
            arr.push_back(v);
        } while (consume(','));
        return consume(']');
    }

    bool parse_feature(FeatureRef& f) {
        if (!consume('{')) return false;
        bool got_name = false, got_edges = false, got_ratios = false;
        while (!peek('}')) {
            std::string key;
            if (!parse_string(key)) return false;
            if (!consume(':')) return false;
            if (key == "name") {
                if (!parse_string(f.name)) return false;
                got_name = true;
            } else if (key == "edges") {
                if (!parse_double_array(f.edges)) return false;
                got_edges = true;
            } else if (key == "ref_ratios") {
                if (!parse_double_array(f.ref_ratios)) return false;
                got_ratios = true;
            } else {
                if (!skip_value()) return false;
            }
            if (!consume(',')) break;
        }
        return consume('}') && got_name && got_edges && got_ratios;
    }

    bool parse(ReferenceProfile& out) {
        if (!consume('{')) return false;
        bool got_version = false, got_window = false, got_features = false;
        while (!peek('}')) {
            std::string key;
            if (!parse_string(key)) return false;
            if (!consume(':')) return false;
            if (key == "version") {
                if (!parse_int(out.version)) return false;
                got_version = true;
            } else if (key == "window_size") {
                if (!parse_int(out.window_size)) return false;
                got_window = true;
            } else if (key == "features") {
                if (!consume('[')) return false;
                out.features.clear();
                if (!peek(']')) {
                    do {
                        FeatureRef f;
                        if (!parse_feature(f)) return false;
                        out.features.push_back(std::move(f));
                    } while (consume(','));
                }
                if (!consume(']')) return false;
                got_features = true;
            } else {
                if (!skip_value()) return false;
            }
            if (!consume(',')) break;
        }
        return consume('}') && got_version && got_window && got_features;
    }
};

bool validate(const ReferenceProfile& prof) {
    if (prof.version != 1) return false;
    if (prof.window_size <= 0) return false;
    if (prof.features.empty()) return false;
    for (const auto& f : prof.features) {
        if (f.edges.size() < 2) return false;
        if (f.ref_ratios.size() != f.edges.size() - 1) return false;
        // Edges must be strictly increasing: upper_bound in driftmon_observe requires it.
        for (size_t i = 1; i < f.edges.size(); ++i)
            if (f.edges[i] <= f.edges[i - 1]) return false;
        double sum = 0.0;
        for (double r : f.ref_ratios) {
            if (r < 0.0) return false;
            sum += r;
        }
        if (std::fabs(sum - 1.0) > 0.01) return false;
    }
    return true;
}

}  // namespace

bool load_reference(const std::string& path, ReferenceProfile& out) {
    std::ifstream file(path);
    if (!file) return false;
    std::string src((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    Parser parser{src.c_str(), src.c_str() + src.size()};
    ReferenceProfile tmp;
    if (!parser.parse(tmp)) return false;
    parser.skip_ws();
    if (parser.p != parser.end) return false;  // trailing garbage after top-level object
    if (!validate(tmp)) return false;
    out = std::move(tmp);
    return true;
}

}  // namespace dm
