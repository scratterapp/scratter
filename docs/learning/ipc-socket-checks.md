# Who may talk to the daemon: the IPC socket checks

The daemon takes orders over a socket file (see [ipc-channel.md](ipc-channel.md)).
Whoever can connect can tell it what to play, and once Apple Music is wired in
(#4) the daemon will hold your login too. So only you, the user who started it,
should get in. This page covers the three checks in
`src/pal/linux/unix_seqpacket_ipc.cpp` that make sure of that, and why the
socket file's own `0600` isn't the whole story.

## Users, owners and permission bits, quickly

Every process runs as a user, a number called the uid (yours is probably 1000;
`id -u` prints it). Every file has an owner uid and nine permission bits: read,
write and execute for the owner, for the file's group, and for everyone else.
`ls -l` shows them as `rwx------`, and in octal that's `0700`: 7 (rwx) for the
owner, 0 for the group, 0 for others.

For a directory the bits mean slightly different things: "read" is listing the
names in it, "write" is creating, deleting and renaming entries in it, and
"execute" is going through it to reach something inside. That "write" is the
one that matters here. Deleting or replacing a file is a change to the
*directory*, not the file, so the file's own permissions don't stop it.

OSTEP covers this in chapter 39 ("Interlude: Files and Directories"), in the
part on permission bits. You haven't got there yet; it's worth reading this page
again after.

Which uid a process has comes from the process API you've read about in
chapter 5: a child made with `fork()` starts as the same user as its parent, and
`exec()` keeps it. Only root may switch to another user with `setuid()`. The
tests below use exactly that: fork a child, have it `setuid()` to the user
`nobody`, and see whether the daemon lets it in.

## Check 1: the folder the socket lives in

The socket file goes in `$XDG_RUNTIME_DIR`, which logind sets to
`/run/user/<your uid>` when you log in, owned by you with mode `0700`. If the
folder really is like that, nobody else can even reach the socket file inside.

But `$XDG_RUNTIME_DIR` is only an environment variable, and it can point
anywhere: a typo in a shell config, a container that sets it to `/tmp`, a
script that makes a fresh folder with the default `0755`. If the folder lets
another user write to it, that user can delete our socket file and put their
own in its place. The UI then connects to *their* program and sends it its
commands, and later its login. Our file's `0600` doesn't help: the attacker
never opens our file, they replace it.

So before using the folder, `check_runtime_dir()` calls `stat()` on it and
refuses it unless:

- it is a directory (otherwise `not_a_directory`),
- its owner is us (`st_uid == geteuid()`), and
- the group and "everyone else" bits are all zero (`st_mode & 077 == 0`).

Either of the last two failing gives `permission_denied`. The daemon then exits
with a message saying what the folder should look like, and amctl (or am-ui)
refuses to connect. Refusing is the point: falling back to some other folder
would just move the problem.

`stat()` follows symlinks, so `$XDG_RUNTIME_DIR` can be a link to a private
folder. What matters is the folder the socket ends up in.

## Check 2: who is connecting (`accept()`)

When a program connects to a Unix socket, the kernel writes down who that
program is: its process id, user and group. The daemon can read that back with
`getsockopt(fd, SOL_SOCKET, SO_PEERCRED, ...)`, which fills in a `struct ucred`
(`pid`, `uid`, `gid`). The connecting program can't fake it: the kernel took it
from the process itself, not from anything the program sent.

`accept()` now does that for every new connection, and if the uid isn't ours,
it closes the connection and returns `permission_denied`. The daemon logs
"turned away a client running as another user" and carries on with anyone else
who is waiting. The other side sees a hang-up.

Normally this check never fires, because the folder and the file already keep
other users out. It's there in case they don't: someone runs `chmod 666` on the
socket while debugging, or a future change gets the folder check wrong. Two
locks on the door, so one mistake doesn't open it. People call this "defence in
depth".

## Check 3: who is listening (`connect()`)

`SO_PEERCRED` works in the other direction too. For the program that connects,
it reports who called `listen()` on the other end. So `connect()` checks that
the daemon it reached runs as us, and fails with `permission_denied` otherwise.

With check 1 in place, only we (or root) can have made a socket in our folder,
so this is again belt and braces. It's also what the Windows side already
does, where it matters much more: Windows pipes have one name space for the
whole machine, so another user can create the daemon's pipe name first.
`connect()` behaves the same on both now, which keeps am-ui free of platform
special cases.

## What about root?

Root's uid is 0, not ours, so the peer check turns root away too. That doesn't
make the daemon safe from root: root ignores permission bits and can read the
daemon's memory with a debugger. Nothing a normal program does keeps root out,
and we don't try.

## The tests

In `tests/test_unix_seqpacket_ipc.cpp`:

- "a runtime dir other users can open is refused" tries `0755`, `0750`,
  `0711` and friends, and expects `permission_denied` from both `listen()` and
  `connect()`. Then `0700` works.
- "a runtime dir that isn't a directory is refused" points the variable at a
  plain file and at a missing path.
- "a symlink to a private runtime dir is accepted".
- Three tests need a second user, so they need root: a folder owned by
  `nobody`; a client that `fork()`s, `setuid()`s to `nobody` and connects
  (after the test loosens the socket file to `0666` by hand, so only the peer
  check is left to stop it); and a listener run by `nobody` in our folder.
  Without root they print `AM_TEST_SKIPPED`, which ctest shows as "Skipped". CI
  runs as a normal user, so run these yourself now and then:

  ```sh
  sudo ./build/linux-debug/tests/test_unix_seqpacket_ipc
  ```

`tests/test_daemon.cpp` has "the daemon refuses a runtime dir other users can
open", which starts the real daemon in a `0755` folder and expects it to exit
with code 1 (setup failed).

One thing those tests turned up: the soak test's ctest entry made its runtime
folder with CMake's `file(MAKE_DIRECTORY)`, which gives `0755`. The daemon now
refuses that, so `tests/CMakeLists.txt` sets it to `0700`.

## Things to try

- Run `ls -ld $XDG_RUNTIME_DIR` and check it's `drwx------` and yours.
- Start `scratterd` with `XDG_RUNTIME_DIR=/tmp` and read what it says.
- Make a folder with `mkdir`, look at its mode, then `chmod 700` it and start
  the daemon there with `XDG_RUNTIME_DIR` pointing at it.
- With the daemon running, `ss -xlp | grep scratterd` lists its socket and the
  process behind it.

## Read more

- `man 7 unix`, the `SO_PEERCRED` part
- `man 2 stat`, `man 7 inode` (the `st_mode` bits)
- The XDG Base Directory spec, the `$XDG_RUNTIME_DIR` paragraph: it says the
  folder "MUST be owned by the user, and he MUST be the only one having read
  and write access to it. Its Unix access mode MUST be 0700."
- OSTEP chapter 5 (fork and exec) and chapter 39 (permission bits)
