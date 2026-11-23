#include "itch_inspect.hpp"
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32) && defined(_MSC_VER)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {
std::string utf8_argument(const wchar_t* argument) {
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        argument, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) throw std::runtime_error("invalid Unicode command-line argument");
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument, -1,
        result.data(), required, nullptr, nullptr) != required) {
        throw std::runtime_error("Unicode command-line conversion failed");
    }
    result.resize(static_cast<std::size_t>(required - 1));
    return result;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> arguments;
        for (int i = 1; i < argc; ++i) arguments.push_back(utf8_argument(argv[i]));
        return nasdaq_itch::run_itch_inspect(arguments, std::cout, std::cerr);
    } catch (const std::exception& exception) {
        std::cerr << "Inspection setup failed: " << exception.what() << '\n';
        return 1;
    }
}
#else
int main(int argc, char** argv) {
    try {
        std::vector<std::string> arguments;
        for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
        return nasdaq_itch::run_itch_inspect(arguments, std::cout, std::cerr);
    } catch (const std::exception& exception) {
        std::cerr << "Inspection setup failed: " << exception.what() << '\n';
        return 1;
    }
}
#endif
