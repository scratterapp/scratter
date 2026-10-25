# Release checklist

Go through this before publishing any build other people can download: a GitHub
release, an AUR package, a Windows installer. Nothing has been released yet, and
the first three items are open questions, not chores. None of this is legal advice;
[`LEGAL.md`](../LEGAL.md) is the public notice, and the spec's Non-goals section
(`SPECIFICATION.md`, just before section 6) lists what the project won't do.

## Blockers

These have to be settled before the first public binary.

- [ ] **Widevine is not in the release.** No `libwidevinecdm.so` or
  `widevinecdm.dll` in the archive, the package or the installer, and nothing
  that downloads it for the user. The daemon uses the copy already on the user's
  machine (spec 3.1.1). Google licenses the CDM, and only its licensees may hand
  it out.
- [ ] **AAC patent licensing is settled.** Our CEF build turns on Chromium's
  AAC decoder, and AAC is covered by patents. Shipping a binary that contains the
  decoder needs an answer first (spec 3.1.1 and the open decision in
  [`CLAUDE.md`](../CLAUDE.md)).
- [ ] **Apple's terms are accounted for.** The developer token comes from our own
  Apple Developer account, never the token Apple's web player uses, and the key
  that signs it is not in the repo or the release. Apple's terms say its services
  should be used through Apple's software; every third-party client lives with
  that, but the README should be honest about it and the app should stay inside
  the Non-goals (one stream per account, no scraping, respect `429` rate limits).

## Licenses for what we bundle

- [ ] **Third-party notices file.** A binary release includes the license text of
  everything compiled or bundled into it: CEF and Chromium (which carries its own
  long list), SDL3, Dear ImGui, Catch2 only if it ends up in a shipped binary (it
  shouldn't, it's test-only), and any library added later (libcurl, simdjson).
  Name it `THIRD_PARTY_NOTICES.md` or similar and ship it next to the binaries.
- [ ] **Fonts can be redistributed.** Any font bundled with `am-ui` has a license
  that allows shipping it (the OFL, used by most Google Fonts, does; many system
  and commercial fonts don't). Dear ImGui's built-in font is fine.
- [ ] **Images and icons are ours or licensed.** The logo and icons in
  `docs/assets/` and the `.desktop` icon are our own work. No Apple logo, no
  Apple Music icon, no album art bundled in the release.

## What we say about it

- [ ] **No claim that Apple makes or endorses it.** The README, release notes,
  AUR description and app's About text say Scratter is unofficial and not
  affiliated with Apple (as `LEGAL.md` already does). Use "Apple Music" only to
  describe what it plays ("a client for Apple Music"), never as part of a name.
- [ ] **No promises we can't back.** Don't claim lossless, Dolby Atmos or "better
  quality than the official app". The stream is 256 kbps AAC, the same as the web
  player.

## Privacy note

- [ ] **A short privacy section in the README** (or a `PRIVACY.md`) that says, in
  plain words: the Apple Music login token is kept in the system keyring on your
  own machine (the login keyring on Linux, Credential Manager on Windows), the app
  talks to Apple and nobody else, there is no telemetry or analytics, and logging
  out deletes the token.
- [ ] **Check that it's true.** Logging out really deletes the token from the
  vault, and a network capture of a normal session shows only Apple's servers.

## Accessibility

- [ ] **Keyboard navigation.** Every control in `am-ui` can be reached and used
  with the keyboard alone (Tab to move, Enter or Space to press, arrows for
  sliders), and the focused control is visible. Dear ImGui supports this with
  `ImGuiConfigFlags_NavEnableKeyboard`.
- [ ] **Readable contrast.** Text against its background meets at least 4.5:1
  (the WCAG AA level) in the default theme, and the UI stays usable when scaled up.

## Security

The October 2026 security audit found these. Most need code that doesn't exist
yet (Apple Music in the daemon, `src/net/`), so they're checks for then.

- [ ] **CEF is current.** The release's CEF is built from the newest stable
  CEF branch, at most two weeks behind Chrome's latest security update, and the
  release notes name the version. Chromium fixes security bugs every week or
  two, so plan to put out a new build after each Chrome security release.
- [ ] **The GPU decision is made.** Since PR #31 the daemon runs Chromium's GPU
  and audio code unsandboxed, in the same process as the login
  (`--in-process-gpu`, `AudioServiceOutOfProcess` off). Before a release that
  plays Apple Music, either put the GPU back in its own process or write down
  why we keep it.
- [ ] **The browser is fenced in.** The page may only navigate to Apple's
  hosts (no `file://`), popups are blocked or limited to Apple's sign-in,
  permission requests are refused except what Widevine needs, there are no
  downloads, no remote debugging port and no `--ignore-certificate-errors`.
- [ ] **No secrets in logs.** Only expected lines from the page's console reach
  the daemon's log, and a full session's log (daemon, `cef.log`) doesn't contain
  the Music-User-Token or the developer token.
- [ ] **Network code is strict.** libcurl verifies TLS certificates, only talks
  to Apple's hosts, and has timeouts and size limits; the JSON parser in
  `src/net/` has a fuzzer in CI like the IPC one.
- [ ] **The release build is hardened.** Compiler hardening flags are on
  (`_FORTIFY_SOURCE`, `_GLIBCXX_ASSERTIONS`, stack protector, full RELRO, PIE;
  on Windows `/guard:cf` and `/CETCOMPAT`), and `checksec --file=scratterd`
  confirms it on the shipped binary.
- [ ] **Crash dumps are decided.** A crash dump holds the token. Either mark
  release builds non-dumpable or say in the privacy note where dumps go.
- [ ] **Downloads can be checked.** Each release lists SHA-256 checksums of its
  files, and the release tag is signed.
- [ ] **GitHub's safety switches are on.** Secret scanning with push
  protection, Dependabot alerts, private vulnerability reporting (which
  `SECURITY.md` points to), and passing CI required before a merge to `main`.

## Last checks

- [ ] No credentials, tokens, keys, CEF binaries or Widevine files anywhere in the
  release or the repo history (`.gitignore` covers the usual names; check anyway).
- [ ] `LICENSE` (MIT) and `LEGAL.md` are in the release.
