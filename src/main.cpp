#include "hawken/cli.hpp"
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                                            nullptr, 0, nullptr, nullptr);
        if (size <= 0) return 64;
        std::string value(static_cast<std::size_t>(size), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                                 value.data(), size, nullptr, nullptr)) return 64;
        value.pop_back();
        args.push_back(std::move(value));
    }
    return hawken::run_cli(args, std::cout, std::cerr);
}
#else
int main(int argc, char* argv[]) {
    return hawken::run_cli(std::vector<std::string>(argv + 1, argv + argc), std::cout, std::cerr);
}
#endif
