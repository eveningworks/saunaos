#include <algorithm>
#include <cctype>
#include <stdexcept>
#include "registry.h"

std::string shout(const std::string &s) {
    if (s.empty()) throw std::invalid_argument("empty string");
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    return r + "!";
}

static int len_plugin(int n) { return (int)shout(std::string((size_t)n, 'x')).size() + twice(n); }
static Registrar reg_text("text", len_plugin);
