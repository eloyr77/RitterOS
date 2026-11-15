/*
 * Test: C++ on RitterOS (43c): libc++, libc++abi and libunwind. Plain
 * standard C++23, so it builds on the Mac as well, against Apple's libc++:
 * every "cxxtest:" line must be the same there (cxx_shell_test compares
 * them). `cxxtest FOLDER` checks static constructors and destructors,
 * exceptions (thrown through several frames, caught by type and base,
 * rethrown, nested, std::exception_ptr carried to another thread,
 * noexcept's terminate left out), RTTI (typeid, dynamic_cast), the
 * containers and algorithms, std::string and std::format, iostreams and
 * string streams, files (fstream, std::filesystem in FOLDER), threads
 * (std::thread, mutexes, condition variables, atomics, std::async,
 * thread_local with a destructor), chrono, std::regex, <random>'s
 * engines, to_chars/from_chars, std::optional/variant/any, operator new
 * failing (std::bad_alloc), and std::print. "cxxtest-own:" lines are
 * RitterOS's alone.
 */
#include <algorithm>
#include <any>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <print>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <variant>
#include <vector>

#define T "cxxtest: "

/* --- static construction and destruction --- */
static std::string order;

struct Global {
    const char *name;
    explicit Global(const char *n) : name(n) { order += std::string("+") + n; }
    ~Global() { std::printf(T "destroyed %s after main (%s)\n", name, order.c_str()); }
};
static Global first("first");
static Global second("second");

/* --- exceptions --- */
struct Base : std::exception {
    const char *what() const noexcept override { return "base"; }
};
struct Derived : Base {
    int code;
    explicit Derived(int c) : code(c) {}
    const char *what() const noexcept override { return "derived"; }
};

static int destructed;
struct Guard {
    ~Guard() { destructed++; }
};

static int __attribute__((noinline)) deep(int n)
{
    Guard g;
    if (n == 0)
        throw Derived(42);
    return deep(n - 1) + 1;
}

static void exceptions()
{
    try {
        deep(10);
    } catch (const Base &b) {
        const Derived *d = dynamic_cast<const Derived *>(&b);
        std::printf(T "caught %s by its base, code %d, %d frames unwound\n", b.what(), d ? d->code : -1,
                    destructed);
    }
    try {
        try {
            throw std::runtime_error("inner");
        } catch (const std::exception &e) {
            std::printf(T "inner caught: %s\n", e.what());
            throw;
        }
    } catch (const std::runtime_error &e) {
        std::printf(T "rethrown and caught again: %s\n", e.what());
    }
    try {
        throw 7;
    } catch (const char *) {
        std::printf(T "wrong handler\n");
    } catch (int v) {
        std::printf(T "an int caught: %d\n", v);
    }
    try {
        try {
            throw std::logic_error("first");
        } catch (...) {
            std::throw_with_nested(std::runtime_error("second"));
        }
    } catch (const std::runtime_error &e) {
        std::printf(T "nested: %s", e.what());
        try {
            std::rethrow_if_nested(e);
        } catch (const std::logic_error &inner) {
            std::printf(" around %s\n", inner.what());
        }
    }
    std::exception_ptr carried;
    std::thread t([&] {
        try {
            throw std::out_of_range("from the thread");
        } catch (...) {
            carried = std::current_exception();
        }
    });
    t.join();
    try {
        std::rethrow_exception(carried);
    } catch (const std::out_of_range &e) {
        std::printf(T "exception_ptr across threads: %s\n", e.what());
    }
    try {
        std::vector<int> v(3);
        (void)v.at(5);
    } catch (const std::out_of_range &) {
        std::printf(T "vector::at out of range\n");
    }
    try {
        std::size_t huge = std::numeric_limits<std::size_t>::max() / 2;
        char *p = new char[huge];
        std::printf(T "no bad_alloc %p\n", (void *)p);
    } catch (const std::bad_alloc &) {
        std::printf(T "bad_alloc for a huge new\n");
    }
}

/* --- RTTI --- */
struct Shape {
    virtual ~Shape() = default;
    virtual double area() const = 0;
};
struct Square : Shape {
    double s;
    explicit Square(double x) : s(x) {}
    double area() const override { return s * s; }
};
struct Circle : Shape {
    double r;
    explicit Circle(double x) : r(x) {}
    double area() const override { return 3.0 * r * r; }
};

static void rtti()
{
    std::vector<std::unique_ptr<Shape>> shapes;
    shapes.push_back(std::make_unique<Square>(2));
    shapes.push_back(std::make_unique<Circle>(1));
    int squares = 0;
    double total = 0;
    for (auto &s : shapes) {
        squares += dynamic_cast<Square *>(s.get()) != nullptr;
        total += s->area();
    }
    const Shape &one = *shapes[0], &two = *shapes[1];
    std::printf(T "rtti: %d square, area %.1f, typeid same %d, differ %d\n", squares, total,
                typeid(one) == typeid(Square), typeid(two) != typeid(Square));
}

/* --- the library --- */
static void library()
{
    std::vector<int> v{5, 3, 9, 1, 7};
    std::sort(v.begin(), v.end());
    std::map<std::string, int> m{{"b", 2}, {"a", 1}, {"c", 3}};
    std::unordered_map<int, std::string> u;
    for (int i = 0; i < 100; i++)
        u[i] = std::to_string(i * i);
    std::set<std::string> words;
    std::istringstream in("the quick brown fox jumps over the lazy dog");
    for (std::string w; in >> w;)
        words.insert(w);
    std::string joined;
    for (auto &[k, val] : m)
        joined += k + "=" + std::to_string(val) + ";";
    std::printf(T "sorted %d %d %d %d %d, map %s, unordered %zu %s, %zu words, sum %d\n", v[0], v[1], v[2], v[3],
                v[4], joined.c_str(), u.size(), u[12].c_str(), words.size(), std::accumulate(v.begin(), v.end(), 0));
    std::string s = std::format("{:>8}|{:<6}|{:08.3f}|{:#x}|{}", "right", "left", 3.14159, 255, true);
    std::ostringstream os;
    os << std::hex << 48879 << ' ' << std::dec << 1.5 << ' ' << std::string_view("view");
    std::printf(T "format %s, stream %s\n", s.c_str(), os.str().c_str());
    char buf[64];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), 0.1 + 0.2);
    *end = 0;
    double back = 0;
    std::from_chars(buf, end, back);
    std::printf(T "to_chars %s, from_chars same %d\n", buf, back == 0.1 + 0.2);
    std::optional<int> none, some = 4;
    std::variant<int, std::string> var = std::string("text");
    std::any a = 3.5;
    std::printf(T "optional %d %d, variant %s, any %.1f\n", none.has_value(), *some, std::get<std::string>(var).c_str(),
                std::any_cast<double>(a));
    std::regex date(R"((\d{4})-(\d{2})-(\d{2}))");
    std::smatch found;
    std::string text = "built on 2026-11-11, tested";
    bool ok = std::regex_search(text, found, date);
    std::printf(T "regex %d: %s/%s/%s, replaced: %s\n", ok, found[3].str().c_str(), found[2].str().c_str(),
                found[1].str().c_str(), std::regex_replace(text, std::regex("[aeiou]"), "*").c_str());
    std::mt19937 gen(12345);
    std::uniform_int_distribution<int> dist(1, 6);
    std::string rolls;
    for (int i = 0; i < 10; i++)
        rolls += std::to_string(gen()) + (i < 9 ? "," : "");
    std::random_device rd;
    std::printf(T "mt19937 %s; random_device %d\n", rolls.c_str(), rd() != rd() || rd() != rd());
    std::cout << T << "iostream " << 42 << ' ' << 2.5 << ' ' << std::boolalpha << true << std::endl;
}

/* --- files --- */
static void files(const std::string &folder)
{
    namespace fs = std::filesystem;
    fs::path base = fs::path(folder) / "cxxfs";
    std::error_code ec;
    fs::remove_all(base, ec);
    bool made = fs::create_directories(base / "a" / "b");
    {
        std::ofstream out(base / "a" / "one.txt");
        out << "line one\nline two\n";
    }
    std::ofstream(base / "a" / "b" / "two.txt") << "2";
    std::ifstream in(base / "a" / "one.txt");
    std::string first_line;
    std::getline(in, first_line);
    std::vector<std::string> names;
    for (auto &e : fs::recursive_directory_iterator(base))
        names.push_back(fs::relative(e.path(), base).generic_string() + (e.is_directory() ? "/" : ""));
    std::sort(names.begin(), names.end());
    std::string list;
    for (auto &n : names)
        list += n + " ";
    auto size = fs::file_size(base / "a" / "one.txt");
    fs::rename(base / "a" / "one.txt", base / "renamed.txt");
    bool moved = fs::exists(base / "renamed.txt") && !fs::exists(base / "a" / "one.txt");
    auto removed = fs::remove_all(base);
    std::printf(T "filesystem: made %d, read '%s', listed %s, size %ju, renamed %d, removed %ju, gone %d\n", made,
                first_line.c_str(), list.c_str(), (uintmax_t)size, moved, (uintmax_t)removed, !fs::exists(base));
    try {
        (void)fs::file_size(base / "nope");
    } catch (const fs::filesystem_error &e) {
        std::printf(T "filesystem_error: %s\n", e.code() == std::errc::no_such_file_or_directory ? "ENOENT" : "other");
    }
}

/* --- threads --- */
static thread_local std::string per_thread = "main";
static std::atomic<int> local_destructors;
struct Local {
    ~Local() { local_destructors++; }
};
static thread_local Local local_one;

static void threads()
{
    std::mutex m;
    std::condition_variable cv;
    int ready = 0;
    std::atomic<long> counter{0};
    std::vector<std::thread> pool;
    for (int i = 0; i < 4; i++)
        pool.emplace_back([&, i] {
            (void)local_one;
            per_thread = "worker" + std::to_string(i);
            for (int k = 0; k < 10000; k++)
                counter.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lock(m);
            ready++;
            cv.notify_all();
        });
    {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return ready == 4; });
    }
    for (auto &t : pool)
        t.join();
    auto f = std::async(std::launch::async, [] { return std::string("from async"); });
    std::promise<int> p;
    std::future<int> fut = p.get_future();
    std::thread setter([&p] { p.set_value(17); });
    int got = fut.get();
    setter.join();
    std::printf(T "threads: counter %ld, ready %d, %s, promise %d, main's thread_local %s, %d destructors\n",
                counter.load(), ready, f.get().c_str(), got, per_thread.c_str(), local_destructors.load());
    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf(T "chrono: slept at least 100 ms %d, under 2 s %d\n", took >= 100, took < 2000);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: cxxtest FOLDER\n");
        return 2;
    }
    std::printf(T "constructed before main: %s\n", order.c_str());
    exceptions();
    rtti();
    library();
    files(argv[1]);
    threads();
    std::print(T "print {} {:.2f}\n", "done", 1.0);
    std::fflush(stdout);
    return 0;
}
