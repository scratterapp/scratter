// launch_ui() on Windows (src/pal/interfaces/process_launch.hpp, issue #41).
//
// CreateProcessW starts the window. Windows has no zombies: once both handles
// it hands back are closed, an exited process is gone for good, so unlike the
// Linux version this needs nothing from the reactor.
//
// Four things are set up on the way:
//
//   - The command line. Windows passes a program one string, not a list, and
//     the program's C runtime splits it again (CommandLineToArgvW's rules).
//     quote() writes each argument so it splits back into exactly that
//     argument: spaces, quotes and backslashes included.
//   - Text. Our strings are UTF-8 and CreateProcessW wants UTF-16, so every
//     argument is converted. One that isn't valid UTF-8 is refused
//     (invalid_argument) rather than passed on mangled.
//   - Ctrl-C. A debug am-ui is a console program, so it would share the
//     daemon's console, and a Ctrl-C meant for a daemon running in a terminal
//     would close the window too. CREATE_NEW_PROCESS_GROUP turns Ctrl-C off in
//     the window, the way POSIX_SPAWN_SETSID keeps it out of the window's
//     way on Linux. A release am-ui is a GUI program with no console at all.
//   - Standard handles. Standard input reads NUL; output and error go where
//     the daemon's go (its console, or a file it was redirected to). They are
//     passed as copies that the window may inherit, and
//     PROC_THREAD_ATTRIBUTE_HANDLE_LIST makes them the only handles it
//     inherits: without the list, CreateProcessW with inheritance on hands the
//     window every inheritable handle the daemon has.

#include "process_launch.hpp"

#include <windows.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace am::pal {
namespace {

std::error_code os_error(DWORD code) noexcept {
    return {static_cast<int>(code), std::system_category()};
}

std::unexpected<std::error_code> last_error() {
    const DWORD code = GetLastError();
    // The two ways a program can be missing, as the interface names them.
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    }
    return std::unexpected(os_error(code));
}

// Closes a handle on every way out. Null and INVALID_HANDLE_VALUE are both
// "nothing here".
class Handle {
  public:
    Handle() noexcept = default;
    explicit Handle(HANDLE handle) noexcept : handle_(handle) {}
    Handle(Handle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Handle& operator=(Handle&&) = delete;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (valid()) {
            CloseHandle(handle_);
        }
    }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

  private:
    HANDLE handle_ = nullptr;
};

// UTF-8 to UTF-16, refusing what isn't UTF-8.
std::expected<std::wstring, std::error_code> widen(std::string_view text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        wide.data(), size);
    return wide;
}

// Appends `arg` so the C runtime's splitting gives it back unchanged. In
// quotes, backslashes are only special right before a quote: 2n backslashes
// and a quote are n backslashes and the end of the quoted part, 2n+1 and a
// quote are n backslashes and a literal quote. So each quote in the argument
// gets its backslashes doubled plus one more, and the backslashes before the
// closing quote get doubled.
void quote(std::wstring& line, std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        line += arg;
        return;
    }
    line += L'"';
    std::size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            line.append(backslashes * 2 + 1, L'\\');
        } else {
            line.append(backslashes, L'\\');
        }
        backslashes = 0;
        line += c;
    }
    line.append(backslashes * 2, L'\\');
    line += L'"';
}

// A copy of one of the daemon's standard handles that the window may
// inherit, or nothing if the daemon hasn't got one (started without a
// console, say). The daemon's own handle stays as it was.
Handle inheritable_copy(DWORD which) {
    HANDLE own = GetStdHandle(which);
    if (own == nullptr || own == INVALID_HANDLE_VALUE) {
        return {};
    }
    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), own, GetCurrentProcess(), &copy, 0, TRUE,
                         DUPLICATE_SAME_ACCESS)) {
        return {};
    }
    return Handle(copy);
}

} // namespace

std::expected<void, std::error_code> launch_ui(Reactor& /*reactor*/,
                                               const std::filesystem::path& program,
                                               std::span<const std::string> args) {
    // The program goes first on the command line too, as argv[0]. It's
    // always quoted: a path can have spaces but never a quote.
    std::wstring line;
    quote(line, program.native());
    for (const std::string& arg : args) {
        auto wide = widen(arg);
        if (!wide) {
            return std::unexpected(wide.error());
        }
        line += L' ';
        quote(line, *wide);
    }

    SECURITY_ATTRIBUTES inherit{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                .lpSecurityDescriptor = nullptr,
                                .bInheritHandle = TRUE};
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                             OPEN_EXISTING, 0, nullptr));
    if (!input.valid()) {
        return last_error();
    }
    Handle output = inheritable_copy(STD_OUTPUT_HANDLE);
    Handle error = inheritable_copy(STD_ERROR_HANDLE);

    // The handle list may not name a handle twice, and output and error are
    // separate copies even when they're the same console, so each is listed
    // once.
    std::vector<HANDLE> passed{input.get()};
    if (output.valid()) {
        passed.push_back(output.get());
    }
    if (error.valid()) {
        passed.push_back(error.get());
    }

    SIZE_T list_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size); // fails, gives the size
    auto list_bytes = std::make_unique<std::byte[]>(list_size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_bytes.get());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &list_size)) {
        return last_error();
    }
    struct ListGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~ListGuard() { DeleteProcThreadAttributeList(list); }
    } list_guard{list};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, passed.data(),
                                   passed.size() * sizeof(HANDLE), nullptr, nullptr)) {
        return last_error();
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = output.get();
    startup.StartupInfo.hStdError = error.get();
    startup.lpAttributeList = list;

    // lpApplicationName is the full path, so Windows doesn't search for the
    // program; a missing one fails here with ERROR_FILE_NOT_FOUND or
    // ERROR_PATH_NOT_FOUND. CreateProcessW may write to the command line, so
    // it gets our own string.
    PROCESS_INFORMATION started{};
    if (!CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, TRUE,
                        CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                        &startup.StartupInfo, &started)) {
        return last_error();
    }
    // The window runs on its own; the daemon keeps nothing of it. Closing
    // these doesn't stop it, it only means the daemon can't wait for it.
    CloseHandle(started.hThread);
    CloseHandle(started.hProcess);
    return {};
}

} // namespace am::pal
