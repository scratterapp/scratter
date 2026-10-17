// Windows file-mapping shared memory for ShmMapping (spec section 3.2.2).
// Windows only.
//
// A named, pagefile-backed section stands in for the POSIX object the Linux
// side uses: CreateFileMappingW(INVALID_HANDLE_VALUE, ...) makes it, the UI
// opens it with OpenFileMappingW, and MapViewOfFile maps it into the process.
// Two processes that map the same section see the same physical pages.
//
// Two things keep the page private to one user. The name lives in the per
// session "Local\" namespace, and the section is created with a security
// descriptor that names the current user as owner and whose DACL grants only
// that user access, so another user can neither open our page nor plant a fake
// one under our name. The explicit owner matters because a process with an
// elevated token would otherwise create objects owned by BUILTIN\Administrators
// rather than the user.
//
// Name lifetime differs from Linux. A section's name resolves only while some
// handle to it is open; a mapped view keeps the memory, not the name. So the
// creator keeps its section handle in native_handle_ for the mapping's life,
// while a reader closes its handle before returning and relies on the view
// alone. The OS closes a crashed daemon's handles, so no name is left behind.

#include "shm_mapping.hpp"

#include <windows.h>

#include <aclapi.h>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace am::pal {
namespace {

std::error_code os_error(DWORD err) noexcept {
    return {static_cast<int>(err), std::system_category()};
}

std::error_code generic(std::errc e) noexcept { return std::make_error_code(e); }

// A plain name: not empty, and no separator or NUL that could escape the
// "Local\" namespace or truncate the name.
bool is_valid_name(std::string_view name) noexcept {
    return !name.empty() && name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos && name.find('\0') == std::string_view::npos;
}

// UTF-8 -> UTF-16. MB_ERR_INVALID_CHARS rejects any ill-formed input (overlong
// forms, lone or encoded surrogates, code points past U+10FFFF), which the
// caller turns into invalid_argument.
std::expected<std::wstring, std::error_code> to_utf16(std::string_view name) noexcept {
    const int len = static_cast<int>(name.size());
    const int wide =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), len, nullptr, 0);
    if (wide <= 0) {
        return std::unexpected(generic(std::errc::invalid_argument));
    }
    std::wstring out(static_cast<std::size_t>(wide), L'\0');
    const int got =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), len, out.data(), wide);
    if (got != wide) {
        return std::unexpected(generic(std::errc::invalid_argument));
    }
    return out;
}

// "<daemon>_telemetry_v2" -> "Local\<daemon>_telemetry_v2".
std::expected<std::wstring, std::error_code> os_name(std::string_view name) noexcept {
    auto wide = to_utf16(name);
    if (!wide) {
        return std::unexpected(wide.error());
    }
    return std::wstring(L"Local\\") + *wide;
}

// Closes a HANDLE on scope exit unless released.
struct HandleGuard {
    HANDLE h = nullptr;
    explicit HandleGuard(HANDLE handle) noexcept : h(handle) {}
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    ~HandleGuard() {
        if (h != nullptr) {
            ::CloseHandle(h);
        }
    }
    void release() noexcept { h = nullptr; }
};

// Unmaps a view on scope exit unless released.
struct ViewGuard {
    void* p = nullptr;
    explicit ViewGuard(void* view) noexcept : p(view) {}
    ViewGuard(const ViewGuard&) = delete;
    ViewGuard& operator=(const ViewGuard&) = delete;
    ~ViewGuard() {
        if (p != nullptr) {
            ::UnmapViewOfFile(p);
        }
    }
    void release() noexcept { p = nullptr; }
};

// The current process user's SID, written into `buf` (which must be at least
// sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE bytes and suitably aligned).
std::expected<PSID, std::error_code> current_user_sid(void* buf, DWORD buf_len) noexcept {
    DWORD got = 0;
    if (::GetTokenInformation(::GetCurrentProcessToken(), TokenUser, buf, buf_len, &got) == 0) {
        return std::unexpected(os_error(::GetLastError()));
    }
    return static_cast<TOKEN_USER*>(buf)->User.Sid;
}

// Fails with permission_denied unless the section behind `h` is owned by
// `user`. Needs READ_CONTROL on the handle.
std::expected<void, std::error_code> verify_owner(HANDLE h, PSID user) noexcept {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD rc = ::GetSecurityInfo(h, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                                       nullptr, nullptr, nullptr, &sd);
    if (rc != ERROR_SUCCESS) {
        return std::unexpected(os_error(rc));
    }
    const bool ours = owner != nullptr && ::EqualSid(owner, user) != 0;
    ::LocalFree(sd);
    if (!ours) {
        return std::unexpected(generic(std::errc::permission_denied));
    }
    return {};
}

// The page-rounded size of the whole section behind the view at `data`.
std::expected<std::size_t, std::error_code> view_size(void* data) noexcept {
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(data, &mbi, sizeof(mbi)) == 0) {
        return std::unexpected(os_error(::GetLastError()));
    }
    return mbi.RegionSize;
}

} // namespace

std::expected<ShmMapping, std::error_code> ShmMapping::create(std::string_view name,
                                                              std::size_t size) noexcept {
    if (!is_valid_name(name) || size == 0) {
        return std::unexpected(generic(std::errc::invalid_argument));
    }
    auto wide_name = os_name(name);
    if (!wide_name) {
        return std::unexpected(wide_name.error());
    }

    alignas(TOKEN_USER) unsigned char user_buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    auto user = current_user_sid(user_buf, sizeof(user_buf));
    if (!user) {
        return std::unexpected(user.error());
    }

    // A DACL with a single ACE granting the current user full section access,
    // and nothing to anyone else.
    alignas(DWORD) unsigned char
        acl_buf[sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE];
    auto* dacl = reinterpret_cast<PACL>(acl_buf);
    if (::InitializeAcl(dacl, sizeof(acl_buf), ACL_REVISION) == 0 ||
        ::AddAccessAllowedAce(dacl, ACL_REVISION, FILE_MAP_ALL_ACCESS, *user) == 0) {
        return std::unexpected(os_error(::GetLastError()));
    }

    SECURITY_DESCRIPTOR sd;
    if (::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) == 0 ||
        ::SetSecurityDescriptorOwner(&sd, *user, FALSE) == 0 ||
        ::SetSecurityDescriptorDacl(&sd, TRUE, dacl, FALSE) == 0) {
        return std::unexpected(os_error(::GetLastError()));
    }
    // Protect the DACL so no inheritable ACE from elsewhere can widen it.
    ::SetSecurityDescriptorControl(&sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    ULARGE_INTEGER wanted;
    wanted.QuadPart = size;
    HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                          wanted.HighPart, wanted.LowPart, wide_name->c_str());
    const DWORD create_err = ::GetLastError();
    if (mapping == nullptr) {
        return std::unexpected(os_error(create_err));
    }
    HandleGuard mapping_guard{mapping};
    const bool existed = create_err == ERROR_ALREADY_EXISTS;

    // A reused section keeps its original owner and size. Refuse one that
    // belongs to another user, then one too small to hold `size` (Windows
    // can't grow a section).
    if (existed) {
        HANDLE probe = ::OpenFileMappingW(READ_CONTROL, FALSE, wide_name->c_str());
        if (probe == nullptr) {
            return std::unexpected(os_error(::GetLastError()));
        }
        HandleGuard probe_guard{probe};
        if (auto owned = verify_owner(probe, *user); !owned) {
            return std::unexpected(owned.error());
        }
    }

    void* view = ::MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
    if (view == nullptr) {
        return std::unexpected(os_error(::GetLastError()));
    }
    ViewGuard view_guard{view};

    auto region = view_size(view);
    if (!region) {
        return std::unexpected(region.error());
    }
    if (*region < size) {
        return std::unexpected(generic(std::errc::result_out_of_range));
    }

    view_guard.release();
    mapping_guard.release();
    // owned_name_ stays empty on Windows: the handle, not the name, is what the
    // mapping must release.
    return ShmMapping(view, size, true, std::string{}, mapping);
}

std::expected<ShmMapping, std::error_code> ShmMapping::open_read_only(std::string_view name,
                                                                      std::size_t size) noexcept {
    if (!is_valid_name(name) || size == 0) {
        return std::unexpected(generic(std::errc::invalid_argument));
    }
    auto wide_name = os_name(name);
    if (!wide_name) {
        return std::unexpected(wide_name.error());
    }

    alignas(TOKEN_USER) unsigned char user_buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    auto user = current_user_sid(user_buf, sizeof(user_buf));
    if (!user) {
        return std::unexpected(user.error());
    }

    // READ_CONTROL lets verify_owner() read the owner; ERROR_FILE_NOT_FOUND maps
    // to no_such_file_or_directory, so a missing region reads as "daemon not
    // running".
    HANDLE mapping = ::OpenFileMappingW(FILE_MAP_READ | READ_CONTROL, FALSE, wide_name->c_str());
    if (mapping == nullptr) {
        return std::unexpected(os_error(::GetLastError()));
    }
    HandleGuard mapping_guard{mapping};

    if (auto owned = verify_owner(mapping, *user); !owned) {
        return std::unexpected(owned.error());
    }

    void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        return std::unexpected(os_error(::GetLastError()));
    }
    ViewGuard view_guard{view};

    auto region = view_size(view);
    if (!region) {
        return std::unexpected(region.error());
    }
    if (*region < size) {
        return std::unexpected(generic(std::errc::result_out_of_range));
    }

    view_guard.release();
    // mapping_guard still closes the handle here: the view alone keeps the
    // memory, and a reader must not keep the name alive. The reader stores no
    // native handle.
    return ShmMapping(view, size, false, std::string{}, nullptr);
}

void ShmMapping::reset() noexcept {
    if (data_ != nullptr) {
        ::UnmapViewOfFile(data_);
    }
    if (native_handle_ != nullptr) {
        ::CloseHandle(native_handle_);
    }
    data_ = nullptr;
    size_ = 0;
    writable_ = false;
    owned_name_.clear();
    native_handle_ = nullptr;
}

} // namespace am::pal