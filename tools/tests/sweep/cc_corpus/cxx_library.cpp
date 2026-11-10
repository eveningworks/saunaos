// The C++ standard library as a program uses it: containers, algorithms, strings and
// streams, <format>, <chrono> arithmetic, smart pointers, std::variant/optional,
// <random> with a fixed seed, <regex>, std::thread and std::filesystem.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <ranges>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

int main() {
    std::vector<int> v(30);
    std::iota(v.begin(), v.end(), -10);
    std::reverse(v.begin(), v.end());
    std::stable_sort(v.begin(), v.end(), [](int a, int b) { return std::abs(a) < std::abs(b); });
    std::cout << "vector";
    for (int i = 0; i < 8; i++) std::cout << ' ' << v[i];
    std::cout << " sum " << std::accumulate(v.begin(), v.end(), 0L) << '\n';
    auto evens = v | std::views::filter([](int x) { return x % 2 == 0; }) | std::views::transform([](int x) { return x * x; });
    std::cout << "ranges";
    for (int x : evens | std::views::take(5)) std::cout << ' ' << x;
    std::cout << '\n';

    std::map<std::string, int> m{ { "zeta", 1 }, { "alpha", 2 }, { "mid", 3 } };
    m["beta"] = 4;
    m.erase("mid");
    std::cout << "map";
    for (auto &[k, val] : m) std::cout << ' ' << k << '=' << val;
    std::cout << '\n';
    std::unordered_map<int, std::string> um;
    for (int i = 0; i < 1000; i++) um[i * 7] = std::to_string(i);
    std::set<int> st(v.begin(), v.end());
    std::deque<int> dq{ 1, 2, 3 };
    dq.push_front(0);
    dq.push_back(4);
    std::list<int> ls{ 5, 3, 9, 1 };
    ls.sort();
    std::cout << "containers " << um.size() << ' ' << um[700] << ' ' << st.size() << ' ' << *st.begin() << ' '
              << dq.front() << dq.back() << ' ' << ls.front() << ls.back() << '\n';

    std::string s = "The quick brown fox";
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    std::string_view sv(s);
    std::cout << "string " << s << ' ' << sv.substr(4, 5) << ' ' << s.find("FOX") << ' ' << s.rfind('O') << '\n';
    std::ostringstream os;
    os << std::setw(8) << std::setfill('*') << 42 << '|' << std::hex << std::showbase << 255 << '|' << std::dec
       << std::fixed << std::setprecision(3) << 3.14159 << '|' << std::scientific << 12345.678 << '|'
       << std::boolalpha << true;
    std::cout << "ostream " << os.str() << '\n';
    std::istringstream is("12 3.5 word 0x1f");
    int i1;
    double d1;
    std::string w1;
    int h1;
    is >> i1 >> d1 >> w1 >> std::hex >> h1;
    std::cout << "istream " << i1 << ' ' << d1 << ' ' << w1 << ' ' << h1 << '\n';
    std::cout << std::format("format [{:>8}] [{:<6}|] [{:08.3f}] [{:#x}] [{:e}] [{}]\n", "right", "left", -3.14159,
                             255, 0.00012, std::string_view("sv"));

    using namespace std::chrono;
    auto dur = hours(2) + minutes(30) + seconds(15);
    std::cout << "chrono " << duration_cast<seconds>(dur).count() << ' ' << duration_cast<minutes>(dur).count() << ' '
              << (milliseconds(1500) + microseconds(250)).count() << '\n';
    year_month_day ymd{ year(2024), month(2), day(29) };
    sys_days sd = ymd;
    year_month_day next = sys_days(sd + days(1));
    std::cout << "calendar " << sd.time_since_epoch().count() << ' ' << static_cast<unsigned>(next.month()) << '/'
              << static_cast<unsigned>(next.day()) << ' ' << ymd.ok() << '\n';
    auto t0 = steady_clock::now();
    std::cout << "steady " << (steady_clock::now() >= t0) << '\n';

    auto up = std::make_unique<std::vector<int>>(5, 7);
    std::shared_ptr<int> sp = std::make_shared<int>(99);
    std::weak_ptr<int> wp = sp;
    auto sp2 = sp;
    std::cout << "smart " << up->size() << ' ' << sp.use_count() << ' ' << *wp.lock() << ' ';
    sp.reset();
    sp2.reset();
    std::cout << wp.expired() << '\n';

    std::variant<int, std::string, double> var = std::string("text");
    std::string seen = std::visit([](auto &&x) -> std::string {
        if constexpr (std::is_same_v<std::decay_t<decltype(x)>, std::string>) return "string:" + x;
        else return "other";
    }, var);
    std::optional<int> none, some = 5;
    std::cout << "variant " << seen << ' ' << var.index() << ' ' << none.value_or(-1) << ' ' << *some << '\n';
    std::function<int(int)> fact = [&](int n) { return n <= 1 ? 1 : n * fact(n - 1); };
    std::cout << "function " << fact(10) << '\n';

    std::mt19937 gen(12345);
    std::uniform_int_distribution<int> dist(1, 6);
    std::cout << "random";
    for (int i = 0; i < 6; i++) std::cout << ' ' << dist(gen);
    std::mt19937_64 g64(1);
    std::cout << ' ' << g64() << '\n';

    std::regex re(R"((\w+)@(\w+)\.com)");
    std::string text = "mail alice@example.com and bob@test.com now";
    std::cout << "regex";
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
        std::cout << ' ' << (*it)[1] << '/' << (*it)[2];
    std::cout << ' ' << std::regex_replace(std::string("a1b22c333"), std::regex("[0-9]+"), "#") << '\n';

    std::atomic<long> total{ 0 };
    std::mutex mu;
    std::vector<int> order;
    std::vector<std::thread> pool;
    for (int t = 0; t < 4; t++)
        pool.emplace_back([&, t] {
            for (int k = 0; k < 10000; k++) total += t + 1;
            std::lock_guard<std::mutex> g(mu);
            order.push_back(t);
        });
    for (auto &th : pool) th.join();
    std::sort(order.begin(), order.end());
    std::cout << "thread " << total << ' ' << order.size() << ' ' << order.front() << order.back() << '\n';

    namespace fs = std::filesystem;
    fs::path dir = "cxl_dir";
    fs::create_directories(dir / "sub");
    { std::ofstream(dir / "sub" / "a.txt") << "hello file\n"; }
    std::ifstream in(dir / "sub" / "a.txt");
    std::string line;
    std::getline(in, line);
    std::cout << "filesystem " << line << ' ' << fs::file_size(dir / "sub" / "a.txt") << ' '
              << fs::exists(dir / "nope") << ' ' << (dir / "sub" / "a.txt").extension() << ' ';
    std::cout << fs::remove_all(dir) << '\n';
    return 0;
}
