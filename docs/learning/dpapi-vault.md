# The Windows vault, explained

This covers `src/pal/win32/dpapi_vault.cpp` (issue #10): the Windows side of
the `CredentialVault` interface from [credential-vault.md](credential-vault.md).
It saves the daemon's two tokens in Credential Manager, the same place Windows
keeps saved network and Git passwords, and encrypts each one with DPAPI first.
The Linux side is [keyring-vault.md](keyring-vault.md).

## Two pieces: a store and a lock

Windows has two separate tools for this, and the vault uses both:

1. **Credential Manager** is the *store*. It keeps small secrets for the
   logged-in user, each looked up by a "target name". You can see them in
   Control Panel > Credential Manager > Windows Credentials, or with
   `cmdkey /list`. It's a system service (part of `lsass`), and its files are
   encrypted on disk.
2. **DPAPI** (the Data Protection API, `CryptProtectData`) is the *lock*. It
   encrypts bytes with a key derived from your Windows password and only hands
   them back to a program running as you, on this machine. No key for us to
   keep: Windows keeps it.

```
vault->store()  ->  CryptProtectData  ->  CredWriteW  ->  Credential Manager
   (our code)        (DPAPI, crypt32)      (advapi32)       (lsass, on disk)
```

Why both, when Credential Manager already encrypts its files? Because any
program running as you can call `CredReadW` and get a generic credential back
in plain text, and password-dumping tools do exactly that. With
DPAPI on top, what comes back is a blob only DPAPI can open. That doesn't stop
a program running as you, which could call DPAPI too, but it stops anything
that only gets a copy of the credential (a backup, a dump tool, another
account), and it's what the spec asks for (section 3.3.3).

## What a credential looks like

| Part | Value |
| --- | --- |
| type | `CRED_TYPE_GENERIC` (`cmdkey` shows it as `LegacyGeneric`) |
| target name | `scratter/music-user-token` (service, a `/`, the name) |
| blob | the token, encrypted by DPAPI |
| persistence | `CRED_PERSIST_LOCAL_MACHINE`: kept across logons, never copied to other machines |

`scratter` is `AM_APP_NAME`; if the name ever changes, new credentials get it
too.

Linux tells two services apart with two attributes. Credential Manager only has
the one target name, so the service and the name are joined with a `/`. That
only works if the join can't be ambiguous: service `a/b` with name `c` and
service `a` with name `b/c` would both make `a/b/c`. So on Windows a name can't
contain `/` (it's `invalid_argument`). The two real names never do, and since
the name is everything after the last `/`, no two services can share a target.

Writing the same target again replaces the credential instead of adding a
second one.

## The entropy trick

`CryptProtectData` takes an optional extra input called *entropy*: bytes that
have to be passed in again, exactly, to decrypt. The vault passes the target
name. So a blob copied from `scratter/developer-token` to
`scratter/music-user-token`, or to another service's target, fails to decrypt
(`ERROR_INVALID_DATA`), even though it's the same user on the same machine.
It costs nothing and closes off "swap the blobs around" tricks.

## What each function does

| Function | Windows calls | Notes |
| --- | --- | --- |
| `open()` | `CredReadW` of `<service>/` | "Not found" means Credential Manager answered |
| `store()` | `CryptProtectData`, then `CredWriteW` | `CRYPTPROTECT_UI_FORBIDDEN`: never a prompt |
| `load()` | `CredReadW`, then `CryptUnprotectData` | `ERROR_NOT_FOUND` means nothing saved |
| `remove()` | `CredDeleteW` | `ERROR_NOT_FOUND` isn't an error |

Why `open()` reads something: like on Linux, the interface says `open()` is
where "there's nowhere to keep secrets" shows up. A normal desktop logon always
has a credential store, but some logons don't (a network logon, or a service
running without a user profile), and there every call fails with
`ERROR_NO_SUCH_LOGON_SESSION`. Reading a target nothing is saved under checks
that once, up front: `ERROR_NOT_FOUND` means it's there, and
`ERROR_NO_SUCH_LOGON_SESSION` becomes `function_not_supported`, which is what
makes the tests skip instead of fail.

Other failures carry Windows' own error code (`GetLastError()` in
`system_category`), as the header says.

## How big a token can be

Credential Manager caps a blob at `CRED_MAX_CREDENTIAL_BLOB_SIZE`, 2560 bytes,
and DPAPI adds a header, padding and a signature to whatever it encrypts.
Measured on Windows 11:

| Secret | Encrypted |
| --- | --- |
| 1 byte | 230 bytes |
| 300 bytes | 518 bytes |
| 1000 bytes | 1222 bytes |
| 2300 bytes | 2518 bytes |
| 2400 bytes | 2630 bytes (too big) |

So a secret can be about 2300 bytes. Apple's developer token and
Music-User-Token are a few hundred each. A bigger one fails with
`std::errc::value_too_large` instead of being cut short. That error isn't in
the header's list yet, because Linux has no such limit.

## Memory

`load()` zeroes DPAPI's decrypted copy (`SecureZeroMemory`) before freeing it,
the same thing libsecret's `secret_password_free` does on Linux. The
`std::string` it returns isn't wiped; [credential-vault.md](credential-vault.md)
says why that's accepted.

## Trying it

```powershell
ctest --preset windows-msvc-debug -R vault
.\build\windows-msvc-debug\tests\manual\vault_demo.exe
```

The demo saves `scratter-demo/music-user-token`, reads it back and waits for
Enter. While it waits, `cmdkey /list | findstr scratter` from another window
shows it, and so does Credential Manager in Control Panel.

## Read next

- Microsoft's "CryptProtectData function" and "CREDENTIALW structure" pages,
  for the flags and limits used here.
- [keyring-vault.md](keyring-vault.md), for the same interface over D-Bus.
