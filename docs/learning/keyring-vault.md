# The Linux login keyring vault, explained

This covers `src/pal/linux/secret_vault.cpp` (issue #53): the Linux side of the
`CredentialVault` interface from [credential-vault.md](credential-vault.md). It
saves the daemon's two tokens in your desktop's login keyring, the same place
your browser and Wi-Fi passwords live.

## The pieces between us and the keyring

Four layers sit between `vault->store(...)` and the encrypted file on disk:

1. **D-Bus.** Linux desktop programs talk to each other over a message bus
   called D-Bus. Each login session has one "session bus": a small daemon
   that every program connects to, then sends messages to other programs by
   name, like `org.freedesktop.secrets`. Think of it as a switchboard. It's the
   same bus media keys (MPRIS, #56) will use.
2. **The Secret Service.** A standard D-Bus API for storing secrets. GNOME
   Keyring, KWallet and KeePassXC all answer to the name
   `org.freedesktop.secrets` and speak it, so one piece of code works with
   whichever one your desktop runs.
3. **The keyring daemon** (on most desktops `gnome-keyring-daemon`, or KWallet
   on KDE). It keeps "collections" of items. The **Login** collection is
   encrypted with your login password and unlocked automatically when you log
   in to the desktop, which is why no password prompt shows up.
4. **libsecret**, the C library we call. It turns "store this password with
   these attributes" into the right D-Bus messages, and encrypts the secret
   while it's on the bus.

```
vault->store()  ->  libsecret  ->  session bus  ->  gnome-keyring-daemon  ->  login.keyring
   (our code)        (C lib)        (D-Bus)           (Secret Service)        (encrypted file)
```

## What an item looks like

A keyring item is a secret plus a few text **attributes** used to find it again,
and a **label** for people to read. Ours:

| Part | Value |
| --- | --- |
| label | `scratter music-user-token` (service, a space, the name) |
| attribute `service` | `scratter` (the product name, `kVaultService`) |
| attribute `name` | `music-user-token` or `developer-token` |
| attribute `xdg:schema` | `scratter.Credential` (added by libsecret) |
| secret | the token itself |

`scratter` is `AM_APP_NAME`; if the name ever changes, new
items get it too.

Lookups match on attributes, not the label. The **schema** is a name libsecret
stores alongside them (`xdg:schema`) so a lookup only matches items made with
our schema. Without it, another app that also happens to use attributes called
`service` and `name` could see our token, or we its.

"Store again" replaces the item instead of adding a second one, because
libsecret looks for an item with the same attributes first.

## What each function does

| Function | libsecret call | Notes |
| --- | --- | --- |
| `open()` | `secret_service_get_sync`, then `g_dbus_proxy_get_name_owner` | Connects, and checks that some program owns `org.freedesktop.secrets` |
| `store()` | `secret_password_store_sync` into `SECRET_COLLECTION_DEFAULT` | The default collection is Login |
| `load()` | `secret_password_lookup_sync` | `NULL` with no error means nothing saved |
| `remove()` | `secret_password_clear_sync` | "Nothing to delete" isn't an error |

Why `open()` checks for an owner: the interface says `open()` is where "there's
nowhere to keep secrets" shows up. Just creating a libsecret object would
succeed even with no keyring running, and the failure would only appear on the
first `store()`. Every name on D-Bus has an *owner*, the program currently
answering to it. While connecting, D-Bus starts the keyring daemon if one is
installed but not running yet (that's called *activation*). So if
`org.freedesktop.secrets` still has no owner after connecting, there's no
keyring on this machine, and `open()` says `function_not_supported`. A CI
runner with no session bus at all fails one step earlier, while connecting.
None of this unlocks anything, so it never pops up a password dialog.

The first version asked libsecret to open a *session* with the keyring
instead (`SECRET_SERVICE_OPEN_SESSION`), which checks the same thing. But in
libsecret 0.21.8, which Arch ships, that call's failure path frees a plain C
struct with `g_object_unref` (a function meant only for GObjects), so every run
on a machine without a keyring printed `GLib-GObject-CRITICAL: g_object_unref:
assertion 'G_IS_OBJECT (object)' failed`. Harmless, but alarming, and it's
libsecret's bug, not ours, so we stepped around it.

The `_sync` versions block until the keyring answers. That's fine: the daemon
only uses the vault at startup and after a login, never on the audio thread.

## Turning GError into std::error_code

libsecret is a C library built on GLib, and GLib reports failures through a
`GError*`: a little struct with a *domain* (which library), a *code* and a
message, which the caller has to free. Our interface returns `std::error_code`,
so `take_error()` translates and frees it in one place:

| GError | Our error |
| --- | --- |
| `SECRET_ERROR_IS_LOCKED` | `permission_denied` (keyring locked, user didn't unlock it) |
| failed with no GError at all | `permission_denied` (libsecret does this when you dismiss the unlock prompt) |
| D-Bus "service unknown" / "no owner" | `function_not_supported` (keyring daemon went away) |
| `G_IO_ERROR_CANCELLED` | `operation_canceled` |
| anything else | `io_error` |

The GError's text message is dropped; `std::error_code` has no room for it.
If debugging ever needs it, logging it inside `take_error()` is the place.

libsecret takes C strings (text ending at the first `'\0'` byte), so a name or
secret containing `'\0'` would be cut short without anyone noticing. Every
function checks for that first and returns `invalid_argument`.

## The tests, and cleaning up after a crash

`tests/test_credential_vault.cpp` is the shared contract from #52, now running
for real on a desktop. Each test opens a vault with its own fixed service name,
like `scratter-test-round-trip`, and deletes both secret names at the start
and at the end.

Why the start too: this project builds without exceptions, so a failed
`REQUIRE` doesn't unwind back out of the test, it ends the whole test program
on the spot (Catch2 calls `std::terminate()`). Cleanup at the end of the test,
or in a destructor, never runs. Before this change each run used a random
service name, so a crash could leave an item like `scratter-test-3141592653`
in your keyring forever. With a fixed name, the next run deletes whatever the
crashed one left.

On CI there's no keyring, so `open()` fails with `function_not_supported` and
each test prints a warning and stops, like the PipeWire tests do with no audio
server.

## ThreadSanitizer and GLib

libsecret talks D-Bus through GLib, and GLib starts a helper thread called
`gdbus` to do it. GLib isn't compiled with ThreadSanitizer, and its locks are
built from raw `futex` system calls TSan can't see. So when the `gdbus` thread
and our thread take turns touching memory those locks guard, TSan thinks
nothing ordered the two and reports a race that isn't real (about a hundred of
them per run).

`tests/tsan.supp` tells TSan to ignore memory operations made from inside GLib,
GIO, GObject and libsecret. The `linux-tsan` test preset passes it in through
`TSAN_OPTIONS`. Races in our own code are still reported. CI never gets that
far (no session bus), but running `ctest --preset linux-tsan` on your desktop
does.

## Seeing it for yourself

`tests/manual/vault_demo` saves a dummy token as `scratter-demo
music-user-token`, waits for you to press Enter, then deletes it. While it waits,
Seahorse (the "Passwords and Keys" app, `sudo pacman -S seahorse`) shows it in
the Login keyring. On KDE, KWalletManager does the same. The `secret-tool` command
(part of `libsecret`) can read items too:

```sh
secret-tool search --all service scratter
```

## Read next

- [credential-vault.md](credential-vault.md), for the interface this implements.
- [daemon.md](daemon.md), for where the daemon will call the vault once login
  exists (spec section 3.3.3).
