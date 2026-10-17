// Tests for CredentialVault (src/pal/interfaces/credential_vault.hpp).
//
// These are the contract both platforms have to meet: Linux's keyring vault
// (#53) and Windows' Credential Manager vault (#10). Each test uses its own
// throwaway service name, "<product>-test-<test>", so it never touches the real
// saved logins.
//
// Each test deletes its secrets at the end, and again at the start. The second
// one matters because a failed REQUIRE ends the whole test program (exceptions
// are off), so the cleanup at the end never runs; with a fixed name per test,
// the next run removes whatever the crashed one left in the keyring.
//
// Until a platform's vault is written, open() says "not supported" and the
// tests stop there. The same happens on a machine with no password store at
// all, like a CI runner with no keyring daemon.

#include "credential_vault.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

using am::pal::CredentialVault;

namespace {

// Deletes both secrets the tests use.
void remove_test_secrets(CredentialVault& vault) {
    CHECK(vault.remove(am::pal::kDeveloperTokenSecret).has_value());
    CHECK(vault.remove(am::pal::kMusicUserTokenSecret).has_value());
}

// A vault for this test's own service, emptied of anything an earlier run left
// behind. nullopt if this machine has nowhere to keep secrets.
std::optional<CredentialVault> open_or_skip(std::string_view test_name) {
    const std::string service =
        std::string(am::pal::kVaultService) + "-test-" + std::string(test_name);
    auto vault = CredentialVault::open(service);
    if (!vault) {
        // Anything else means the store is there but broken.
        CHECK(vault.error() == std::errc::function_not_supported);
        WARN("no password store here (" << vault.error().message() << "), skipping");
        return std::nullopt;
    }
    CHECK(vault->service() == service);
    remove_test_secrets(*vault);
    return std::move(*vault);
}

} // namespace

TEST_CASE("the secret names come from the product name", "[vault]") {
    CHECK(am::pal::kVaultService == am::common::kAppName);
    CHECK(am::pal::kDeveloperTokenSecret != am::pal::kMusicUserTokenSecret);
}

TEST_CASE("a secret can be stored, loaded, replaced and removed", "[vault]") {
    auto vault = open_or_skip("round-trip");
    if (!vault) {
        return;
    }

    // Nothing saved yet is an empty optional, not an error.
    auto empty = vault->load(am::pal::kMusicUserTokenSecret);
    REQUIRE(empty.has_value());
    CHECK_FALSE(empty->has_value());

    REQUIRE(vault->store(am::pal::kMusicUserTokenSecret, "first-token").has_value());
    auto loaded = vault->load(am::pal::kMusicUserTokenSecret);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == "first-token");

    // Storing again replaces it.
    REQUIRE(vault->store(am::pal::kMusicUserTokenSecret, "second-token").has_value());
    loaded = vault->load(am::pal::kMusicUserTokenSecret);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == "second-token");

    // The two names are separate secrets.
    auto other = vault->load(am::pal::kDeveloperTokenSecret);
    REQUIRE(other.has_value());
    CHECK_FALSE(other->has_value());

    REQUIRE(vault->remove(am::pal::kMusicUserTokenSecret).has_value());
    loaded = vault->load(am::pal::kMusicUserTokenSecret);
    REQUIRE(loaded.has_value());
    CHECK_FALSE(loaded->has_value());

    // Removing it again still succeeds.
    CHECK(vault->remove(am::pal::kMusicUserTokenSecret).has_value());

    remove_test_secrets(*vault);
}

TEST_CASE("two services don't share secrets", "[vault]") {
    auto first = open_or_skip("separate-first");
    auto second = open_or_skip("separate-second");
    if (!first || !second) {
        return;
    }
    REQUIRE(first->store(am::pal::kDeveloperTokenSecret, "only-in-first").has_value());

    auto seen = second->load(am::pal::kDeveloperTokenSecret);
    REQUIRE(seen.has_value());
    CHECK_FALSE(seen->has_value());

    remove_test_secrets(*first);
    remove_test_secrets(*second);
}

TEST_CASE("bad names and secrets are rejected", "[vault]") {
    auto vault = open_or_skip("bad-input");
    if (!vault) {
        return;
    }
    using namespace std::string_view_literals;

    auto stored = vault->store("", "token");
    REQUIRE_FALSE(stored.has_value());
    CHECK(stored.error() == std::errc::invalid_argument);

    stored = vault->store(am::pal::kMusicUserTokenSecret, "");
    REQUIRE_FALSE(stored.has_value());
    CHECK(stored.error() == std::errc::invalid_argument);

    stored = vault->store(am::pal::kMusicUserTokenSecret, "bad\0token"sv);
    REQUIRE_FALSE(stored.has_value());
    CHECK(stored.error() == std::errc::invalid_argument);

    auto loaded = vault->load("");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == std::errc::invalid_argument);

    auto removed = vault->remove("");
    REQUIRE_FALSE(removed.has_value());
    CHECK(removed.error() == std::errc::invalid_argument);

    // None of those should have saved anything, but if one did, don't leave it.
    remove_test_secrets(*vault);
}
