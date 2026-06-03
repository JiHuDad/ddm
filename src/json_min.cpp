// json_min.cpp — minimal recursive-descent JSON parser for reference.json.
//
// Parses exactly the schema in SPEC.md §3. Not a general JSON library.
// Standard library only.
#include "json_min.h"

#include <algorithm>
#include <cerrno>
#include <cfloat>
#include <cmath>
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
                    case '"':  s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/':  s += '/'; break;
                    case 'n':  s += '\n'; break;
                    case 'r':  s += '\r'; break;
                    case 't':  s += '\t'; break;
                    default:   return false;
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
        p = nend;
        return true;
    }

    bool parse_int(int& v) {
        double d;
        if (!parse_number(d)) return false;
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
    if (!validate(tmp)) return false;
    out = std::move(tmp);
    return true;
}

}  // namespace dm
