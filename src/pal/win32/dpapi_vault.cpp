// TODO: Implement the DPAPI + Credential Manager vault. Owner: @kerelosgayed
//
// Until then this is a stub so the daemon links on Windows: open() fails with "not
// supported", so nothing can call the other functions. Replace the whole file
// with the real vault (src/pal/interfaces/credential_vault.hpp, issue #54).

#include "credential_vault.hpp"

namespace am::pal {

std::expected<CredentialVault, std::error_code> CredentialVault::open(std::string_view) noexcept {
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
}

std::expected<void, std::error_code> CredentialVault::store(std::string_view,
                                                            std::string_view) noexcept {
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
}

std::expected<std::optional<std::string>, std::error_code>
CredentialVault::load(std::string_view) noexcept {
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
}

std::expected<void, std::error_code> CredentialVault::remove(std::string_view) noexcept {
    return std::unexpected(std::make_error_code(std::errc::function_not_supported));
}

} // namespace am::pal
