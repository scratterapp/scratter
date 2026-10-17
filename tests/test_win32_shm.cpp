// Windows-only tests for win32_shm.cpp (issue #8). The cross-platform contract
// is covered by test_shm_mapping.cpp; this checks the Windows specifics: the
// owner and DACL on a created section, read-only page protection, the
// page-rounded size check, rejection of ill-formed names, and the per-handle
// name lifetime.

#include "shm_mapping.hpp"

#include <windows.h>

#include <aclapi.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using am::pal::ShmMapping;

constexpr std::size_t kPage = 4096;

std::string unique_name() {
    std::random_device rd;
    return "am_wintest_" + std::to_string(rd()) + "_" + std::to_string(rd());
}

std::wstring os_name(std::string_view name) {
    const int n =
        ::MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), w.data(), n);
    return L"Local\\" + w;
}

// The current process user's SID, copied into `buf`.
PSID current_user_sid(void* buf, DWORD buf_len) {
    DWORD got = 0;
    REQUIRE(::GetTokenInformation(::GetCurrentProcessToken(), TokenUser, buf, buf_len, &got) != 0);
    return static_cast<TOKEN_USER*>(buf)->User.Sid;
}

bool is_elevated() {
    TOKEN_ELEVATION te{};
    DWORD got = 0;
    const BOOL ok =
        ::GetTokenInformation(::GetCurrentProcessToken(), TokenElevation, &te, sizeof(te), &got);
    return ok != 0 && te.TokenIsElevated != 0;
}

DWORD page_protection(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    REQUIRE(::VirtualQuery(p, &mbi, sizeof(mbi)) != 0);
    return mbi.Protect;
}

} // namespace

TEST_CASE("create sets the current user as owner with a single-ACE DACL", "[win32shm]") {
    const std::string name = unique_name();
    auto m = ShmMapping::create(name, kPage);
    REQUIRE(m.has_value());

    alignas(TOKEN_USER) unsigned char user_buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    PSID user = current_user_sid(user_buf, sizeof(user_buf));

    HANDLE h = ::OpenFileMappingW(READ_CONTROL, FALSE, os_name(name).c_str());
    REQUIRE(h != nullptr);

    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    REQUIRE(::GetSecurityInfo(h, SE_KERNEL_OBJECT,
                              OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                              nullptr, &dacl, nullptr, &sd) == ERROR_SUCCESS);

    CHECK(owner != nullptr);
    CHECK(::EqualSid(owner, user) != 0);

    ACL_SIZE_INFORMATION size_info{};
    REQUIRE(::GetAclInformation(dacl, &size_info, sizeof(size_info), AclSizeInformation) != 0);
    REQUIRE(size_info.AceCount == 1);

    ACCESS_ALLOWED_ACE* ace = nullptr;
    REQUIRE(::GetAce(dacl, 0, reinterpret_cast<void**>(&ace)) != 0);
    CHECK(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE);
    CHECK(::EqualSid(reinterpret_cast<PSID>(&ace->SidStart), user) != 0);
    CHECK((ace->Mask & FILE_MAP_ALL_ACCESS) == FILE_MAP_ALL_ACCESS);

    ::LocalFree(sd);
    ::CloseHandle(h);
}

TEST_CASE("the creator maps read-write and a reader maps read-only", "[win32shm]") {
    const std::string name = unique_name();
    auto writer = ShmMapping::create(name, kPage);
    REQUIRE(writer.has_value());
    auto reader = ShmMapping::open_read_only(name, kPage);
    REQUIRE(reader.has_value());

    CHECK(page_protection(writer->bytes().data()) == PAGE_READWRITE);
    CHECK(page_protection(reader->bytes().data()) == PAGE_READONLY);
    CHECK(reader->writable_bytes().empty());
}

TEST_CASE("create refuses an existing region smaller than the request", "[win32shm]") {
    const std::string name = unique_name();
    auto first = ShmMapping::create(name, kPage);
    REQUIRE(first.has_value());

    auto bigger = ShmMapping::create(name, 2 * kPage);
    REQUIRE_FALSE(bigger.has_value());
    CHECK(bigger.error() == std::errc::result_out_of_range);
}

TEST_CASE("ill-formed names are rejected on both functions", "[win32shm]") {
    const std::array<std::string, 8> bad = {
        std::string("\0ab", 3),          // NUL at the start
        std::string("a\0b", 3),          // NUL in the middle
        std::string("ab\0", 3),          // NUL at the end
        std::string("a\xFF"),            // stray continuation byte
        std::string("\xC0\xAF"),         // overlong '/'
        std::string("\xED\xA0\x80"),     // encoded surrogate U+D800
        std::string("\xF4\x90\x80\x80"), // past U+10FFFF
        std::string("\xE2\x82"),         // truncated sequence
    };
    for (const std::string& n : bad) {
        INFO("name length " << n.size());
        CHECK(ShmMapping::create(n, kPage).error() == std::errc::invalid_argument);
        CHECK(ShmMapping::open_read_only(n, kPage).error() == std::errc::invalid_argument);
    }
}

TEST_CASE("two creators keep the name until the last is gone", "[win32shm]") {
    const std::string name = unique_name();
    auto first = ShmMapping::create(name, kPage);
    auto second = ShmMapping::create(name, kPage);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    *first = ShmMapping{}; // destroy the first creator's mapping
    CHECK(ShmMapping::open_read_only(name, kPage).has_value());

    *second = ShmMapping{}; // destroy the last creator's mapping
    CHECK(ShmMapping::open_read_only(name, kPage).error() == std::errc::no_such_file_or_directory);
}

TEST_CASE("a name kept alive only by a reader view is gone", "[win32shm]") {
    const std::string name = unique_name();
    auto writer = ShmMapping::create(name, kPage);
    REQUIRE(writer.has_value());
    writer->writable_bytes()[0] = std::byte{0xAB};

    auto reader = ShmMapping::open_read_only(name, kPage);
    REQUIRE(reader.has_value());

    *writer = ShmMapping{}; // only the reader's view remains

    // The name is released with the creator's handle, even though the view
    // still holds the old pages.
    CHECK(ShmMapping::open_read_only(name, kPage).error() == std::errc::no_such_file_or_directory);

    // A fresh create() under the same name makes a new, zeroed region.
    auto again = ShmMapping::create(name, kPage);
    REQUIRE(again.has_value());
    CHECK(again->bytes()[0] == std::byte{0});
    CHECK(reader->bytes()[0] == std::byte{0xAB});
}

// Catch2's SKIP() works by throwing, and this project builds Catch2 without
// exceptions, where it terminates the process instead (a debug-CRT abort
// dialog). So a test that can't run here prints AM_TEST_SKIPPED and returns, and
// tests/CMakeLists.txt gives ctest a SKIP_REGULAR_EXPRESSION that turns that
// line into a "Skipped" result.
TEST_CASE("a region owned by another principal is refused", "[win32shm]") {
    if (!is_elevated()) {
        WARN("AM_TEST_SKIPPED: needs an elevated token to create a section owned by "
             "Administrators");
        return;
    }

    const std::string name = unique_name();
    const std::wstring wide = os_name(name);

    alignas(TOKEN_USER) unsigned char user_buf[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    PSID user = current_user_sid(user_buf, sizeof(user_buf));

    unsigned char admin_buf[SECURITY_MAX_SID_SIZE];
    DWORD admin_len = sizeof(admin_buf);
    REQUIRE(::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_buf, &admin_len) != 0);
    PSID admin = reinterpret_cast<PSID>(admin_buf);

    // A DACL granting the current user full access, so our own open can read the
    // owner, but with Administrators as the owner.
    alignas(DWORD) unsigned char
        acl_buf[sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE];
    PACL dacl = reinterpret_cast<PACL>(acl_buf);
    REQUIRE(::InitializeAcl(dacl, sizeof(acl_buf), ACL_REVISION) != 0);
    REQUIRE(::AddAccessAllowedAce(dacl, ACL_REVISION, FILE_MAP_ALL_ACCESS, user) != 0);

    SECURITY_DESCRIPTOR sd;
    REQUIRE(::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) != 0);
    REQUIRE(::SetSecurityDescriptorOwner(&sd, admin, FALSE) != 0);
    REQUIRE(::SetSecurityDescriptorDacl(&sd, TRUE, dacl, FALSE) != 0);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    HANDLE foreign =
        ::CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, kPage, wide.c_str());
    if (foreign == nullptr && ::GetLastError() == ERROR_INVALID_OWNER) {
        WARN("AM_TEST_SKIPPED: this token may not make Administrators the owner of an object");
        return;
    }
    REQUIRE(foreign != nullptr);

    CHECK(ShmMapping::create(name, kPage).error() == std::errc::permission_denied);
    CHECK(ShmMapping::open_read_only(name, kPage).error() == std::errc::permission_denied);

    ::CloseHandle(foreign);
}