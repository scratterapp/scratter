# The CredentialVault interface, explained

This covers `src/pal/interfaces/credential_vault.hpp` (issue #52). It is the
contract that `secret_vault.cpp` (#53, Linux) and `dpapi_vault.cpp` (#10,
Windows) fill in. Both are written: see [keyring-vault.md](keyring-vault.md)
for Linux and [dpapi-vault.md](dpapi-vault.md) for Windows.

## What it's for

To play anything, the daemon needs two secrets:

- the **developer token**, which identifies this app to Apple Music's API, and
- the **Music-User-Token**, which Apple hands out when you log in and which says
  "this request is from robbie's subscription".

Logging in every time the daemon starts would be miserable, so the daemon saves
both and loads them on the next start. Saving them in a plain file next to the
config would work, but anything that can read your home directory could copy
them. Every desktop OS already has a proper place for this:

- **Linux:** the login keyring (GNOME Keyring or KWallet), reached through
  libsecret. It's unlocked when you log in to your desktop, and you can see
  what's in it with Seahorse or KWalletManager.
- **Windows:** Credential Manager, with the secret encrypted by DPAPI under
  your Windows account first, so even a copy of the stored blob is useless on
  another account or machine.

The vault interface is the one door to both.

## The whole interface

```cpp
auto vault = CredentialVault::open();               // default service: the app name

vault->store(kMusicUserTokenSecret, token);         // save, replacing the old one
auto token = vault->load(kMusicUserTokenSecret);    // get it back
vault->remove(kMusicUserTokenSecret);               // forget it
```

That's all of it: three verbs and an `open()`. Issue #52's job was to pick
those verbs and pin down what each one does at the edges, so Linux and Windows
can't drift apart.

## Three answers, not two

`load()` has three possible outcomes, and they mean different things to the
daemon:

| Result | Meaning | What the daemon does |
| --- | --- | --- |
| a string | the token is saved | use it |
| nothing saved | first run, or after a log out | open the login window |
| an error | the keyring broke, or the user wouldn't unlock it | log it, then open the login window |

A naive design would return `std::expected<std::string, std::error_code>` and
make "nothing saved" one of the error codes. Then every caller has to remember
which error code is the harmless one. Instead `load()` returns

```cpp
std::expected<std::optional<std::string>, std::error_code>
```

which reads from the outside in: *either it worked or it failed* (`expected`),
and *if it worked, there may or may not be a token* (`optional`). The type
itself tells you there are three cases, and the compiler makes you unwrap both
layers to reach the string:

```cpp
auto token = vault->load(kMusicUserTokenSecret);
if (!token)       { /* error */ }
else if (!*token) { /* nothing saved */ }
else              { use(**token); }   // two stars: one per layer
```

`remove()` makes the same kind of choice: deleting something that isn't there
counts as success, because the caller wanted it gone and it is.

## Why `open()` takes a service name

Every secret is filed under a *service* (the app's name, from `AM_APP_NAME`)
plus a *name* (`developer-token` or `music-user-token`). That's what you see in
Seahorse or Credential Manager, and it's why the service comes from the
product name instead of being typed out: if the name ever changes, the keyring
entries follow.

`open()` lets you pass a different service. The real daemon never does; the
tests do. `tests/test_credential_vault.cpp` opens a vault with a random
throwaway service like `scratter-test-3141592653`, so a test run can never
overwrite or delete your actual saved login.

## Why the vault holds almost nothing

The class has one member: the service name. libsecret and Credential Manager
both look secrets up by name on every call, so there's no connection or handle
to keep open. `ShmMapping` needed move-only RAII because it owned a mapping
that had to be unmapped; the vault owns nothing like that, so it doesn't need
any of it. Adding a pimpl or a handle "just in case" would be an abstraction
with nothing behind it. If #53 or #10 finds it needs one, the header has a
comment marking where it goes.

## Blocking is fine here, but not everywhere

Every call can block. The keyring is another process reached over D-Bus, and
the first call after login can pop up an "unlock your keyring" dialog and wait
for you to type a password. That's fine at startup or right after logging in.
It would be a disaster on the audio thread, which has a few milliseconds per
callback. The header says so, and nothing in the audio path will ever touch the
vault.

## The contract tests, ready before the code

The test file is written against the interface, not either implementation:

- store, load, replace, remove, and remove-again
- the two secret names are separate
- two services don't see each other's secrets
- an empty name, an empty secret or a secret containing a `'\0'` byte is
  `invalid_argument` (libsecret stores C strings, so a NUL would silently cut
  the token short)

The tests were written while both platforms' `open()` still said
`function_not_supported`, so each one prints a warning and stops early when
that happens. That's still what happens on a CI machine with no keyring
daemon. On a desktop the same tests run for real: against the login keyring
since #53, and against Credential Manager since #10. Neither issue had to
invent a test.

## What's deliberately left out

- **Wiping memory.** The token comes back in a plain `std::string`, and its
  bytes stay in freed memory until something overwrites them. A
  zero-on-destroy string type would be tidier, but the same token also lives
  in CEF's memory for as long as music plays, so wiping our copy wouldn't buy
  much.
- **Listing secrets.** The daemon knows the two names it uses. Nothing needs to
  ask "what's in here?"
- **Async calls.** libsecret has async versions, but the daemon only touches the
  vault at startup and after a login, when there's nothing else to do anyway.

## Read next

- Spec section 3.3.3, for when the vault gets used: the cold-boot check and the
  login popup.
- [shm-mapping.md](shm-mapping.md), for the same "one class, one `.cpp` per
  platform, `std::expected` errors" shape with RAII on top.
