// Child process for test_process_launch.cpp: what launch_ui() started on
// Windows, seen from inside.
//
// Writes a report to the file named by AM_LAUNCH_CHILD_OUT, one line each:
//
//   arg <n> <UTF-8 bytes in hex>   for each argument after its own name
//   stdin <bytes it could read>    0: standard input reads NUL
//   done
//
// Hex, so spaces, quotes and backslashes in an argument can't blur the
// report. It's written to a side file and renamed into place, so the test
// never reads half of it.

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <string>

namespace {

std::string narrow(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.pop_back(); // the terminating zero
    return out;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t out[MAX_PATH];
    if (GetEnvironmentVariableW(L"AM_LAUNCH_CHILD_OUT", out, MAX_PATH) == 0) {
        return 1;
    }
    std::string report;
    for (int i = 1; i < argc; ++i) {
        report += "arg " + std::to_string(i) + " ";
        char pair[3];
        for (unsigned char c : narrow(argv[i])) {
            std::snprintf(pair, sizeof(pair), "%02x", c);
            report += pair;
        }
        report += '\n';
    }
    char buffer[64];
    DWORD read = 0;
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &read, nullptr)) {
        read = 0;
    }
    report += "stdin " + std::to_string(read) + "\ndone\n";

    const std::wstring side = std::wstring(out) + L".part";
    HANDLE file = CreateFileW(side.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 2;
    }
    DWORD written = 0;
    WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
    CloseHandle(file);
    return MoveFileExW(side.c_str(), out, MOVEFILE_REPLACE_EXISTING) ? 0 : 3;
}
