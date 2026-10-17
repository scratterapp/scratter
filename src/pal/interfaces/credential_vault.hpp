#pragma once

// Login storage: keeps the daemon's secrets in the operating system's own
// password store (spec section 3.3.3), so the user logs in once and not on
// every start.
//
//   Linux:    the login keyring over libsecret (GNOME Keyring, KWallet), #53
//   Windows:  Credential Manager, with the secret encrypted by DPAPI, #54
//
// The daemon keeps two secrets, named below: the developer token that lets it
// talk to Apple Music's API, and the Music-User-Token Apple hands out when the
// user logs in. Neither ever goes in the repo, a log or the IPC socket.
//
//   auto vault = CredentialVault::open();
//   if (!vault) { log vault.error().message(); carry on without saved logins }
//
//   auto token = vault->load(kMusicUserTokenSecret);
//   if (!token)       { the keyring failed: log it, ask the user to log in }
//   else if (!*token) { nothing saved yet: ask the user to log in }
//   else              { use **token }
//
//   vault->store(kMusicUserTokenSecret, new_token);  // after a login
//   vault->remove(kMusicUserTokenSecret);             // after a 401, or log out
//
// "Nothing saved" isn't an error. load() returns an empty std::optional for
// it, so callers don't have to pick it out of a list of error codes.
//
// Every call can block: the keyring is another process (or, on Windows, a
// system service), and on Linux the first call can pop up an "unlock your
// keyring" dialog and wait for the user. Call it from the daemon's main
// thread at startup or after a login, never from the audio thread.
//
// Errors are std::error_code, like the other interfaces. OS failures carry
// their own code where there is one (GetLastError() in system_category on
// Windows; libsecret's GError on Linux maps to the closest std::errc).
// Compare against std::errc to branch:
//
//   std::errc::function_not_supported  no password store on this machine (no
//                                      keyring daemon running, for example),
//                                      or this platform's version isn't
//                                      written yet
//   std::errc::permission_denied       the keyring is locked and the user
//                                      didn't unlock it
//   std::errc::invalid_argument        an empty name, or a secret that is
//                                      empty or contains a '\0' byte
//
// There is one class and one set of member functions. src/pal/linux/secret_vault.cpp
// defines them on Linux and src/pal/win32/dpapi_vault.cpp on Windows, and CMake
// compiles only one of the two. Nothing here may include a platform header.

#include "common/app_name.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace am::pal {

// Which secrets the daemon keeps. Plain ASCII, so each platform can use them
// as-is in its own naming scheme.
inline constexpr std::string_view kDeveloperTokenSecret = "developer-token";
inline constexpr std::string_view kMusicUserTokenSecret = "music-user-token";

// Every secret is filed under this service name, so it's easy to find (and
// delete) in Seahorse, KWalletManager or Credential Manager. It's the product
// name from AM_APP_NAME, never a hard-coded "scratterd".
//
// How a platform turns (service, name) into its own key is up to it, as long
// as two services never share secrets. Plan so far: Linux stores the two as
// libsecret attributes, Windows uses "<service>/<name>" as the credential's
// target name.
inline constexpr std::string_view kVaultService = common::kAppName;

class CredentialVault {
  public:
    // Checks that the password store is there and returns a vault for
    // `service`. The tests pass a throwaway service name so they never touch
    // the real one. Fails with function_not_supported if there's nowhere to
    // keep secrets on this machine.
    [[nodiscard]] static std::expected<CredentialVault, std::error_code>
    open(std::string_view service = kVaultService) noexcept;

    // Saves `secret` under `name`, replacing whatever was there.
    [[nodiscard]] std::expected<void, std::error_code> store(std::string_view name,
                                                             std::string_view secret) noexcept;

    // The secret saved under `name`, or std::nullopt if nothing is.
    [[nodiscard]] std::expected<std::optional<std::string>, std::error_code>
    load(std::string_view name) noexcept;

    // Deletes the secret saved under `name`. Deleting something that isn't
    // there succeeds, so "log out" doesn't need to check first.
    [[nodiscard]] std::expected<void, std::error_code> remove(std::string_view name) noexcept;

    [[nodiscard]] const std::string& service() const noexcept { return service_; }

  private:
    explicit CredentialVault(std::string service) noexcept : service_(std::move(service)) {}

    // Nothing else: both platforms' stores are looked up by name on every
    // call, so there's no handle to keep open. If one ends up needing one,
    // this is where it goes.
    std::string service_;
};

} // namespace am::pal
