// C++ the language: exceptions through destructors and across frames, RTTI, virtual and
// multiple inheritance, templates (variadic, SFINAE, concepts, fold expressions),
// lambdas, move semantics, constexpr, and static objects' construction order.
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <type_traits>
#include <utility>

static int trace_n;
static char trace[256];
static void note(char c) { if (trace_n < 255) trace[trace_n++] = c; }

struct Guard {
    char tag;
    explicit Guard(char t) : tag(t) { note(t); }
    ~Guard() { note((char)(tag - 32)); }
};

struct AppError : std::runtime_error {
    int code;
    AppError(const char *m, int c) : std::runtime_error(m), code(c) {}
};

static int thrower(int depth) {
    Guard g((char)('a' + depth));
    if (depth == 4) throw AppError("deep failure", 42);
    return thrower(depth + 1) + 1;
}

struct Shape {
    virtual ~Shape() = default;
    virtual double area() const = 0;
    virtual const char *name() const { return "shape"; }
};
struct Rect : Shape {
    double w, h;
    Rect(double a, double b) : w(a), h(b) {}
    double area() const override { return w * h; }
    const char *name() const override { return "rect"; }
};
struct Square : Rect {
    explicit Square(double s) : Rect(s, s) {}
    const char *name() const override { return "square"; }
};
struct Named { virtual ~Named() = default; virtual std::string label() const { return "named"; } };
struct Both final : Square, Named {
    Both() : Square(3) {}
    std::string label() const override { return std::string("both:") + name(); }
};

template <typename... T> constexpr auto sum(T... v) { return (v + ... + 0); }
template <typename T> concept Integral = std::is_integral_v<T>;
template <Integral T> constexpr T gcd(T a, T b) { return b == 0 ? a : gcd(b, a % b); }
template <typename T, typename = void> struct has_size : std::false_type {};
template <typename T> struct has_size<T, std::void_t<decltype(std::declval<T>().size())>> : std::true_type {};
template <int N> struct Fib { static constexpr long v = Fib<N - 1>::v + Fib<N - 2>::v; };
template <> struct Fib<1> { static constexpr long v = 1; };
template <> struct Fib<0> { static constexpr long v = 0; };

constexpr int cube(int x) { return x * x * x; }
static_assert(cube(3) == 27);
static_assert(Fib<40>::v == 102334155);

struct Movable {
    std::string s;
    int moved = 0, copied = 0;
    explicit Movable(const char *p) : s(p) {}
    Movable(const Movable &o) : s(o.s), copied(o.copied + 1) {}
    Movable(Movable &&o) noexcept : s(std::move(o.s)), moved(o.moved + 1) {}
};

struct Order { char c; explicit Order(char x) : c(x) { note(x); } };
static Order first_static('1');
static Order second_static('2');

int main() {
    std::printf("statics %.*s\n", trace_n, trace);
    trace_n = 0;
    try {
        thrower(0);
    } catch (const std::runtime_error &e) {
        const AppError *ae = dynamic_cast<const AppError *>(&e);
        std::printf("caught %s code %d unwind %.*s\n", e.what(), ae ? ae->code : -1, trace_n, trace);
    }
    try {
        try { throw 7; } catch (int v) { std::printf("inner %d\n", v); throw; }
    } catch (int v) { std::printf("rethrown %d\n", v); }
    try { throw std::out_of_range("range"); }
    catch (const std::logic_error &e) { std::printf("base-catch %s %s\n", e.what(), typeid(e).name()); }
    bool nested = false;
    try { try { throw 1; } catch (...) { std::throw_with_nested(std::runtime_error("outer")); } }
    catch (const std::exception &e) {
        try { std::rethrow_if_nested(e); } catch (int) { nested = true; }
    }
    std::printf("nested %d noexcept %d\n", nested, noexcept(cube(2)));

    Both b;
    Shape *s = &b;
    Named *n = &b;
    std::printf("virtual %s %.1f %s\n", s->name(), s->area(), n->label().c_str());
    std::printf("cross-cast %d typeid %d\n", dynamic_cast<Named *>(s) == n, typeid(*s) == typeid(Both));
    Rect r(2, 5);
    Shape &ref = r;
    std::printf("bad-cast ");
    try { (void)dynamic_cast<Square &>(ref); } catch (const std::bad_cast &) { std::printf("thrown\n"); }

    std::printf("templates %d %d %ld %d %d\n", sum(1, 2, 3, 4), gcd(84, 36), Fib<50>::v,
                has_size<std::string>::value, has_size<int>::value);
    int base = 10;
    auto adder = [base](int x) mutable { base += x; return base; };
    adder(1);
    auto generic = [](auto a, auto b) { return a * b; };
    std::printf("lambdas %d %g %d\n", adder(5), generic(1.5, 4.0), [&] { return base; }());

    Movable m("payload");
    Movable c = m;
    Movable mv = std::move(m);
    std::printf("move %s copied %d moved %d left-empty %d\n", mv.s.c_str(), c.copied, mv.moved, m.s.empty());
    constexpr int table[] = { cube(1), cube(2), cube(3) };
    std::printf("constexpr %d\n", table[0] + table[1] + table[2]);
    return 0;
}
