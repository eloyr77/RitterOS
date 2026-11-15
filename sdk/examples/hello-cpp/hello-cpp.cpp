// hello-cpp: C++ on RitterOS -- the standard library (strings, containers,
// algorithms, std::format), an exception thrown and caught, and a thread.
#include <algorithm>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static int checked(int n)
{
    if (n < 0)
        throw std::invalid_argument(std::format("{} is negative", n));
    return n;
}

int main(int argc, char **argv)
{
    std::vector<std::string> words(argv + 1, argv + argc);
    std::sort(words.begin(), words.end());
    std::cout << std::format("hello-cpp: {} argument(s):", words.size());
    for (const auto &w : words)
        std::cout << ' ' << w;
    std::cout << '\n';
    try {
        checked(-3);
    } catch (const std::invalid_argument &e) {
        std::cout << "hello-cpp: caught " << e.what() << '\n';
    }
    int from_thread = 0;
    std::thread t([&] { from_thread = 42; });
    t.join();
    std::cout << "hello-cpp: the thread said " << from_thread << std::endl;
    return 0;
}
