// CredentialVault on Windows: Credential Manager, with each secret encrypted by
// DPAPI first (spec section 3.3.3, issue #10).
//
// Credential Manager (the "Windows Credentials" page in Control Panel, or
// `cmdkey /list`) keeps small secrets for the logged-in user, looked up by a
// "target name". Each of ours is one generic credential with the target
// "<service>/<name>", like "scratter/music-user-token". Names can't contain a
// '/', so splitting at the last one always gives back the same service and
// name, and two services can never land on the same target.
//
// Credential Manager already encrypts what it stores, but any program running
// as the user can read a generic credential back in plain text, and it's easy
// to dump. So the blob we hand it is encrypted by DPAPI (CryptProtectData)
// under the user's Windows account: useless to another account or another
// machine, and to anything that copies the credential without also asking
// DPAPI. The target name goes in as DPAPI's "entropy", an extra input the same
// bytes have to come with to be decrypted, so a blob copied to another target
// won't decrypt there either.
//
// Credential Manager caps a blob at CRED_MAX_CREDENTIAL_BLOB_SIZE (2560 bytes),
// and DPAPI adds about 230 bytes to whatever it encrypts, so a secret can be
// about 2300 bytes. Apple's tokens are well under that. A longer one fails with
// value_too_large rather than being cut short.
//
// Every call goes to the Credential Manager service (lsass) and blocks
// briefly; none of them can show a prompt (CRYPTPROTECT_UI_FORBIDDEN).

#include "credential_vault.hpp"

#include <windows.h>

#include <dpapi.h>
#include <wincred.h>

#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace am::pal {
namespace {

std::error_code make_error(std::errc code) noexcept { return std::make_error_code(code); }

std::error_code os_error(DWORD code) noexcept {
    return {static_cast<int>(code), std::system_category()};
}

// A name ends up inside a target name, so it can't contain the '/' that
// separates it from the service. Like on Linux, no '\0' either.
bool is_valid_name(std::string_view name) noexcept {
    return !name.empty() &&
           name.find_first_of(std::string_view("/\0", 2)) == std::string_view::npos;
}

bool is_valid_secret(std::string_view secret) noexcept {
    return !secret.empty() && secret.find('\0') == std::string_view::npos;
}

// UTF-8 to UTF-16, or nothing if it isn't valid UTF-8.
std::optional<std::wstring> widen(std::string_view text) {
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const int length = static_cast<int>(text.size());
    const int wide_length =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (wide_length <= 0) {
        return std::nullopt;
    }
    std::wstring wide(static_cast<size_t>(wide_length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, wide.data(),
                          wide_length);
    return wide;
}

// "<service>/<name>" as Credential Manager wants it.
std::expected<std::wstring, std::error_code> target_name(std::string_view service,
                                                         std::string_view name) {
    auto target = widen(std::string(service) + "/" + std::string(name));
    if (!target || target->size() > CRED_MAX_GENERIC_TARGET_NAME_LENGTH) {
        return std::unexpected(make_error(std::errc::invalid_argument));
    }
    return std::move(*target);
}

// The target name's bytes, as DPAPI's entropy. DATA_BLOB isn't const-correct,
// but DPAPI only reads it.
DATA_BLOB entropy_of(const std::wstring& target) noexcept {
    DATA_BLOB entropy{};
    entropy.cbData = static_cast<DWORD>(target.size() * sizeof(wchar_t));
    entropy.pbData = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(target.data()));
    return entropy;
}

} // namespace

std::expected<CredentialVault, std::error_code>
CredentialVault::open(std::string_view service) noexcept {
    if (service.empty() || service.find('\0') != std::string_view::npos) {
        return std::unexpected(make_error(std::errc::invalid_argument));
    }
    auto probe = target_name(service, "");
    if (!probe) {
        return std::unexpected(probe.error());
    }

    // Look up a target nothing is saved under: "not found" means Credential
    // Manager answered. Logon sessions without a user profile (some services,
    // network logons) have no credential store and fail with
    // ERROR_NO_SUCH_LOGON_SESSION instead.
    PCREDENTIALW found = nullptr;
    if (::CredReadW(probe->c_str(), CRED_TYPE_GENERIC, 0, &found)) {
        ::CredFree(found);
        return CredentialVault(std::string(service));
    }
    const DWORD error = ::GetLastError();
    if (error == ERROR_NOT_FOUND) {
        return CredentialVault(std::string(service));
    }
    if (error == ERROR_NO_SUCH_LOGON_SESSION) {
        return std::unexpected(make_error(std::errc::function_not_supported));
    }
    return std::unexpected(os_error(error));
}

std::expected<void, std::error_code> CredentialVault::store(std::string_view name,
                                                            std::string_view secret) noexcept {
    if (!is_valid_name(name) || !is_valid_secret(secret)) {
        return std::unexpected(make_error(std::errc::invalid_argument));
    }
    // Encrypted, it could only get bigger. This also keeps the size in a DWORD.
    if (secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        return std::unexpected(make_error(std::errc::value_too_large));
    }
    auto target = target_name(service_, name);
    if (!target) {
        return std::unexpected(target.error());
    }

    DATA_BLOB plain{};
    plain.cbData = static_cast<DWORD>(secret.size());
    plain.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()));
    DATA_BLOB entropy = entropy_of(*target);
    DATA_BLOB sealed{};
    if (!::CryptProtectData(&plain, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &sealed)) {
        return std::unexpected(os_error(::GetLastError()));
    }
    if (sealed.cbData > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        ::LocalFree(sealed.pbData);
        return std::unexpected(make_error(std::errc::value_too_large));
    }

    // CRED_PERSIST_LOCAL_MACHINE: kept across logons on this machine, and not
    // copied to other machines with a roaming profile.
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target->data();
    credential.CredentialBlobSize = sealed.cbData;
    credential.CredentialBlob = sealed.pbData;
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    // Writing the same target again replaces the credential.
    const BOOL written = ::CredWriteW(&credential, 0);
    const DWORD error = ::GetLastError();
    ::LocalFree(sealed.pbData);
    if (!written) {
        return std::unexpected(os_error(error));
    }
    return {};
}

std::expected<std::optional<std::string>, std::error_code>
CredentialVault::load(std::string_view name) noexcept {
    if (!is_valid_name(name)) {
        return std::unexpected(make_error(std::errc::invalid_argument));
    }
    auto target = target_name(service_, name);
    if (!target) {
        return std::unexpected(target.error());
    }

    PCREDENTIALW credential = nullptr;
    if (!::CredReadW(target->c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_NOT_FOUND) {
            return std::optional<std::string>{}; // nothing saved
        }
        return std::unexpected(os_error(error));
    }

    DATA_BLOB sealed{};
    sealed.cbData = credential->CredentialBlobSize;
    sealed.pbData = credential->CredentialBlob;
    DATA_BLOB entropy = entropy_of(*target);
    DATA_BLOB plain{};
    const BOOL opened = ::CryptUnprotectData(&sealed, nullptr, &entropy, nullptr, nullptr,
                                             CRYPTPROTECT_UI_FORBIDDEN, &plain);
    // Read before CredFree, which may change it.
    const DWORD error = ::GetLastError();
    ::CredFree(credential);
    if (!opened) {
        // Saved by another Windows account, or the blob was copied from another
        // target: either way it can't be read here.
        return std::unexpected(os_error(error));
    }
    std::string secret(reinterpret_cast<const char*>(plain.pbData), plain.cbData);
    // Zeroes DPAPI's copy before freeing it, like the Linux vault does with
    // libsecret's. Ours is a plain std::string; credential-vault.md says why
    // that's accepted.
    ::SecureZeroMemory(plain.pbData, plain.cbData);
    ::LocalFree(plain.pbData);
    return secret;
}

std::expected<void, std::error_code> CredentialVault::remove(std::string_view name) noexcept {
    if (!is_valid_name(name)) {
        return std::unexpected(make_error(std::errc::invalid_argument));
    }
    auto target = target_name(service_, name);
    if (!target) {
        return std::unexpected(target.error());
    }
    if (!::CredDeleteW(target->c_str(), CRED_TYPE_GENERIC, 0)) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_NOT_FOUND) { // nothing to delete counts as success
            return std::unexpected(os_error(error));
        }
    }
    return {};
}

} // namespace am::pal
