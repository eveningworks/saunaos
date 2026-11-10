// multi_unit: C++ across translation units -- static registration from several
// objects, an exception thrown in one unit and caught in another, inline variables,
// templates instantiated in two units, and a C unit called through extern "C".
#pragma once
#include <string>
#include <vector>

struct Plugin {
    const char *name;
    int (*run)(int);
};
std::vector<Plugin> &registry();
struct Registrar {
    Registrar(const char *name, int (*fn)(int)) { registry().push_back({ name, fn }); }
};
inline int inline_counter = 0;
template <typename T> T twice(T v) { inline_counter++; return v + v; }
std::string shout(const std::string &s);   // in text.cpp, throws on an empty string
int checked_div(int a, int b);             // in math.cpp, throws on b == 0
extern "C" int c_mix(int a, int b);        // in mix.c
