#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include "registry.h"

std::vector<Plugin> &registry() {
    static std::vector<Plugin> r;
    return r;
}

int main() {
    auto &r = registry();
    std::sort(r.begin(), r.end(), [](const Plugin &a, const Plugin &b) { return std::string(a.name) < b.name; });
    for (auto &p : r) std::printf("plugin %s %d\n", p.name, p.run(6));
    int t1 = twice(21);
    std::string t2 = twice(std::string("ab"));
    std::printf("twice %d %s inline %d\n", t1, t2.c_str(), inline_counter);
    try {
        checked_div(1, 0);
    } catch (const std::domain_error &e) {
        std::printf("cross-unit catch %s\n", e.what());
    }
    try {
        shout("");
    } catch (const std::invalid_argument &e) {
        std::printf("cross-unit catch %s\n", e.what());
    }
    std::printf("shout %s c_mix %d\n", shout("quiet").c_str(), c_mix(7, 9));
    return 0;
}
