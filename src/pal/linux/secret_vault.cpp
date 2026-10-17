// CredentialVault on Linux: the login keyring, through libsecret (spec section
// 3.3.3, issue #53).
//
// Desktop keyrings (GNOME Keyring, KWallet, KeePassXC) all speak one D-Bus API
// called the Secret Service (org.freedesktop.secrets). libsecret is the client
// library for it: we hand it a few text attributes and a password, it sends
// them over D-Bus, and the keyring encrypts the password into its own file
// (~/.local/share/keyrings/login.keyring for GNOME Keyring). That file is
// unlocked with your login password when you log in to the desktop.
//
// Each secret is one keyring item with two attributes, "service" (the product
// name, or a test's throwaway name) and "name" ("music-user-token" or
// "developer-token"), filed under our own schema so other apps' items with the
// same attribute names never match. Seahorse shows it as
// "<service> <name>", in the Login keyring.
//
// All the calls are libsecret's blocking *_sync versions; see the header for
// why that's fine here.

#include "credential_vault.hpp"

#include "common/app_name.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <libsecret/secret.h>

namespace am::pal {
namespace {

std::error_code make_error(std::errc code) noexcept { return std::make_error_code(code); }

// The schema says which attributes our items have. Its name is stored with each
// item, and lookups only match items with the same schema name.
const SecretSchema* vault_schema() noexcept {
    static const std::string name = std::string(common::kAppName) + ".Credential";
    static const SecretSchema schema = [] {
        SecretSchema s{};
        s.name = name.c_str();
        s.flags = SECRET_SCHEMA_NONE;
        s.attributes[0] = {"service", SECRET_SCHEMA_ATTRIBUTE_STRING};
        s.attributes[1] = {"name", SECRET_SCHEMA_ATTRIBUTE_STRING};
        return s;
    }();
    return &schema;
}

// Turns libsecret's GError into the error codes the header promises, and frees
// it. libsecret sometimes fails without setting an error, when the user
// dismisses the "unlock your keyring" prompt, so a null error means that.
std::error_code take_error(GError* error) noexcept {
    if (error == nullptr) {
        return make_error(std::errc::permission_denied);
    }
    std::errc code = std::errc::io_error;
    if (error->domain == SECRET_ERROR && error->code == SECRET_ERROR_IS_LOCKED) {
        code = std::errc::permission_denied;
    } else if (error->domain == G_DBUS_ERROR && (error->code == G_DBUS_ERROR_SERVICE_UNKNOWN ||
                                                 error->code == G_DBUS_ERROR_NAME_HAS_NO_OWNER)) {
        // The keyring daemon isn't running (or went away since open()).
        code = std::errc::function_not_supported;
    } else if (error->domain == G_IO_ERROR && error->code == G_IO_ERROR_CANCELLED) {
        code = std::errc::operation_canceled;
    }
    g_error_free(error);
    return make_error(code);
}

// libsecret takes C strings, so a '\0' inside would silently cut the secret
// short. Names have the same problem.
bool is_valid(std::string_view text) noexcept {
    return !text.empty() && text.find('\0') == std::string_view::npos;
}

} // namespace

std::expected<CredentialVault, std::error_code>
CredentialVault::open(std::string_view service) noexcept {
    if (!is_valid(service)) {
        return std::unexpected(make_error(std::errc::invalid_argument));
    }

    // Connect to the Secret Service and check that something answers to it.
    // Creating the connection asks the bus for the service's properties, which
    // also starts the keyring daemon if it's installed but not running yet. If
    // nothing owns the name afterwards, there's no keyring on this machine. A
    // machine with no session bus at all (a CI runner) fails one step earlier.
    // None of this unlocks anything, so it never shows a prompt. libsecret
    // keeps the connection, and the store/load/remove calls below reuse it.
    //
    // We don't pass SECRET_SERVICE_OPEN_SESSION here, though it would check the
    // same thing: libsecret 0.21.8's error path for it frees a plain struct with
    // g_object_unref and logs a GLib-GObject-CRITICAL when there's no keyring.
    GError* error = nullptr;
    SecretService* secret_service = secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &error);
    if (secret_service == nullptr) {
        if (error != nullptr) {
            g_error_free(error);
        }
        return std::unexpected(make_error(std::errc::function_not_supported));
    }
    gchar* owner = g_dbus_proxy_get_name_owner(G_DBUS_PROXY(secret_service));
    g_object_unref(secret_service);
    if (owner == nullptr) {
        return std::unexpected(make_error(std::errc::function_not_supported));
    }
    g_free(owner);
    return CredentialVault(std::string(service));
}

std::expected<void, std::error_code> CredentialVault::store(std::string_view name,
                                                            std::string_view secret) noexcept {
    if (!is_valid(name) || !is_valid(secret)) {
        return std::unexpected(make_error(std::errc::invalid_argument));
    }
    const std::string name_z(name);
    const std::string secret_z(secret);
    // What Seahorse and KWalletManager show for the item.
    const std::string label = service_ + " " + name_z;

    // SECRET_COLLECTION_DEFAULT is the keyring the user logs in with ("Login"
    // in Seahorse). Storing again with the same attributes replaces the item.
    GError* error = nullptr;
    const gboolean stored = secret_password_store_sync(
        vault_schema(), SECRET_COLLECTION_DEFAULT, label.c_str(), secret_z.c_str(), nullptr, &error,
        "service", service_.c_str(), "name", name_z.c_str(), nullptr);
    if (!stored) {
        return std::unexpected(take_error(error));
    }
    return {};
}

std::expected<std::optional<std::string>, std::error_code>
CredentialVault::load(std::string_view name) noexcept {
    if (!is_valid(name)) {
        return std::unexpected(make_error(std::errc::invalid_argument));
    }
    const std::string name_z(name);

    GError* error = nullptr;
    gchar* found = secret_password_lookup_sync(vault_schema(), nullptr, &error, "service",
                                               service_.c_str(), "name", name_z.c_str(), nullptr);
    if (error != nullptr) {
        return std::unexpected(take_error(error));
    }
    if (found == nullptr) {
        return std::optional<std::string>{}; // nothing saved
    }
    std::string secret(found);
    // Zeroes libsecret's copy before freeing it. Ours is a plain std::string;
    // credential-vault.md says why that's accepted.
    secret_password_free(found);
    return secret;
}

std::expected<void, std::error_code> CredentialVault::remove(std::string_view name) noexcept {
    if (!is_valid(name)) {
        return std::unexpected(make_error(std::errc::invalid_argument));
    }
    const std::string name_z(name);

    // Returns false with no error when there was nothing to delete, which
    // counts as success.
    GError* error = nullptr;
    secret_password_clear_sync(vault_schema(), nullptr, &error, "service", service_.c_str(), "name",
                               name_z.c_str(), nullptr);
    if (error != nullptr) {
        return std::unexpected(take_error(error));
    }
    return {};
}

} // namespace am::pal
