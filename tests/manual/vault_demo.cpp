// Saves a dummy secret in your login keyring so you can see it in Seahorse
// (or KWalletManager), then deletes it when you press Enter. This is the hand
// test for issue #53. It's not run by ctest because the point is to look.
//
//   ./build/linux-debug/tests/manual/vault_demo
//
// The item is filed under "<product>-demo", never the real service name, so it
// can't overwrite a real saved login.

#include "credential_vault.hpp"

#include <cstdio>
#include <string>

int main() {
    const std::string service = std::string(am::pal::kVaultService) + "-demo";
    auto vault = am::pal::CredentialVault::open(service);
    if (!vault) {
        std::printf("couldn't reach the keyring: %s\n", vault.error().message().c_str());
        return 1;
    }

    auto stored = vault->store(am::pal::kMusicUserTokenSecret, "not-a-real-token");
    if (!stored) {
        std::printf("store failed: %s\n", stored.error().message().c_str());
        return 1;
    }
    std::printf("Saved \"%s %.*s\" in the Login keyring.\n", service.c_str(),
                static_cast<int>(am::pal::kMusicUserTokenSecret.size()),
                am::pal::kMusicUserTokenSecret.data());

    auto loaded = vault->load(am::pal::kMusicUserTokenSecret);
    if (loaded && *loaded) {
        std::printf("Read it back: %s\n", (*loaded)->c_str());
    }

    std::printf("Look for it in Seahorse now, then press Enter to delete it.\n");
    std::getchar();

    auto removed = vault->remove(am::pal::kMusicUserTokenSecret);
    if (!removed) {
        std::printf("remove failed: %s\n", removed.error().message().c_str());
        return 1;
    }
    std::printf("Deleted.\n");
    return 0;
}
