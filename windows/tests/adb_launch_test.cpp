// Run from a fresh fixture folder containing spaces, &, and non-ASCII text.
// Copy this executable into fixture/adb/adb.exe to act as an isolated fake helper.
#include "adb.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>

Logger logger(Logger::FILE);

int wmain(int argc, wchar_t** argv)
{
    namespace fs = std::filesystem;
    if (fs::path(argv[0]).filename() == L"adb.exe")
    {
        // The helper must never inherit or allocate a console, even if its
        // caller has one. A shell command would fail this check.
        if (GetConsoleWindow()) return 99;
        std::ofstream trace(fs::path(adb::dir()).parent_path() / "invocations.txt", std::ios::app);
        for (int i = 1; i < argc; ++i)
        {
            std::wstring arg(argv[i]);
            trace << (i > 1 ? " " : "") << std::string(arg.begin(), arg.end());
        }
        trace << '\n';
        trace.close();
        if (argc > 1 && std::wstring(argv[1]) == L"fail") return 37;
        if (argc > 1 && std::wstring(argv[1]) == L"wait") Sleep(60000);
        return 0;
    }
    assert(adb::reverse(7969, 7970));
    assert(adb::forward(8555));
    assert(adb::removeReverse(7969));
    assert(adb::removeForward(8555));
    assert(!adb::run(L"fail"));
    auto before = std::chrono::steady_clock::now();
    assert(!adb::run(L"wait", 150));
    assert(std::chrono::steady_clock::now() - before < std::chrono::seconds(3));
    const fs::path helper = fs::path(adb::dir()) / L"adb/adb.exe";
    const fs::path missing = helper.parent_path() / L"adb.saved";
    fs::rename(helper, missing);
    const bool result = adb::run(L"version");
    fs::rename(missing, helper);
    assert(!result);
    std::ifstream trace(fs::path(adb::dir()) / "invocations.txt");
    const std::string actual((std::istreambuf_iterator<char>(trace)), {});
    assert(actual == "reverse --no-rebind tcp:7969 tcp:7970\nforward --no-rebind tcp:8555 tcp:8555\n"
                     "reverse --remove tcp:7969\nforward --remove tcp:8555\nfail\nwait\n");
    std::cout << "PASS: Unicode/spaced/shell-metacharacter path, no helper console, "
                 "USB arguments, exit failures, timeout and missing helper\n";
    return 0;
}
