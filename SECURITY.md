# Security policy

Scratter is an early, unreleased project run by two people in their spare
time. There are no released versions yet, so security fixes go into `main`.

## Reporting a vulnerability

Please don't open a public issue for a security problem. Use GitHub's private
reporting instead: on the repo's **Security** tab, click **Report a
vulnerability**. Only the maintainers see the report.

Helpful things to include: what you found, how to trigger it, which commit or
build you tried, and what an attacker could do with it.

We'll answer within a week, sooner when we can, and tell you what we plan to
do. Once it's fixed we're happy to credit you in the fix's PR, or to leave you
out if you'd rather.

## What counts

Things we want to hear about:

- Ways to read or steal the Apple Music login token the daemon keeps in the
  system keyring.
- Ways for another user on the same machine, a web page, or another program to
  control the daemon, read its telemetry or crash it on purpose.
- Bugs in the embedded browser setup (`src/daemon/cef_host/`) that let a page
  escape where it should be, or load something it shouldn't.
- Memory-safety bugs in code that reads data from outside the process: the IPC
  socket, the shared memory page, D-Bus media controls, or the network.
- Leaked secrets anywhere in the repo or its history.

Not security bugs for this project:

- Ways to get around DRM, or to save decrypted audio. Scratter is a player and
  won't add those (see the Non-goals in `docs/SPECIFICATION.md`).
- Bugs in Chromium or CEF themselves. Report those to the
  [Chromium project](https://www.chromium.org/Home/chromium-security/reporting-security-bugs/);
  we'll still want to know if we're shipping an old version.
- Attacks that need root, or that already run code as your own user (that code
  can ask your keyring for the token directly, the same as for any desktop app).
