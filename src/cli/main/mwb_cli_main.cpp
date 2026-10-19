// mwb-cli entry point. Arguments are taken as UTF-8 (wide command line on Windows) and passed as
// literal strings, never evaluated as shell code (section 31).
#include "mwb/cli/cli.hpp"

#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

std::vector<std::string> utf8_arguments([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    std::vector<std::string> arguments;
#ifdef _WIN32
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int index = 0; wide && index < count; ++index) {
        const int length = WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, nullptr, 0, nullptr, nullptr);
        std::string text(static_cast<std::size_t>(length > 0 ? length - 1 : 0), '\0');
        if (length > 1)
            WideCharToMultiByte(CP_UTF8, 0, wide[index], -1, text.data(), length, nullptr, nullptr);
        arguments.push_back(std::move(text));
    }
    LocalFree(wide);
#else
    for (int index = 0; index < argc; ++index)
        arguments.emplace_back(argv[index]);
#endif
    return arguments;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string standard_output;
    std::string standard_error;
    const int code = mwb::cli::run(utf8_arguments(argc, argv), standard_output, standard_error);
    std::fwrite(standard_output.data(), 1, standard_output.size(), stdout);
    std::fwrite(standard_error.data(), 1, standard_error.size(), stderr);
    return code;
}
