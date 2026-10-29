// Saves a dummy secret in your password store so you can see it there, then
// deletes it when you press Enter. This is the hand test for issues #53 and
// #10. It's not run by ctest because the point is to look.
//
//   Linux:    ./build/linux-debug/tests/manual/vault_demo
//             then look in Seahorse (or KWalletManager), Login keyring
//   Windows:  .\build\windows-msvc-debug\tests\manual\vault_demo.exe
//             then `cmdkey /list | findstr scratter-demo` from another
//             window, or Control Panel > Credential Manager > Windows
//             Credentials
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
        std::printf("couldn't reach the password store: %s\n", vault.error().message().c_str());
        return 1;
    }

    auto stored = vault->store(am::pal::kMusicUserTokenSecret, "not-a-real-token");
    if (!stored) {
        std::printf("store failed: %s\n", stored.error().message().c_str());
        return 1;
    }
    std::printf("Saved \"%s %.*s\" in the password store.\n", service.c_str(),
                static_cast<int>(am::pal::kMusicUserTokenSecret.size()),
                am::pal::kMusicUserTokenSecret.data());

    auto loaded = vault->load(am::pal::kMusicUserTokenSecret);
    if (loaded && *loaded) {
        std::printf("Read it back: %s\n", (*loaded)->c_str());
    }

    std::printf(
        "Look for it now (see the top of vault_demo.cpp), then press Enter to delete it.\n");
    std::getchar();

    auto removed = vault->remove(am::pal::kMusicUserTokenSecret);
    if (!removed) {
        std::printf("remove failed: %s\n", removed.error().message().c_str());
        return 1;
    }
    std::printf("Deleted.\n");
    return 0;
}
