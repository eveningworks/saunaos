#include <stdexcept>
#include "registry.h"

int checked_div(int a, int b) {
    if (b == 0) throw std::domain_error("division by zero");
    return a / b;
}

static int square_plugin(int n) { return n * n; }
static int div_plugin(int n) { return checked_div(100, n) + c_mix(n, 1); }
static Registrar reg_square("square", square_plugin);
static Registrar reg_div("div", div_plugin);
