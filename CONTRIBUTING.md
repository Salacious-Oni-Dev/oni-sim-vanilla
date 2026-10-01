# Contributing

## This repository is frozen

This library is kept as it is. It receives fixes for differences from the game's own
`SimDLL.dll` and nothing else: no new features and no extensions. Development of the simulation
library continues in
[oni-sim-replacement](https://github.com/Salacious-Oni-Dev/oni-sim-replacement).

## Issues

Reports of a difference between this library and the game's own `SimDLL.dll` are welcome in the
issue tracker. Other questions about the SDK belong in
[oni-sdk-docs](https://github.com/Salacious-Oni-Dev/oni-sdk-docs/issues). Security problems are
the exception: see [Security](#security) below.

A useful report gives the release or commit you built, the game build, what you did, what the
game's own library does and what this one does instead, and the game's log (`Player.log`). A
`diffsim` scenario that shows the difference is the most useful report of all.

## Security

Please do not report a security problem in a public issue. Use GitHub's private vulnerability
reporting instead: open this repository's **Security** tab and choose **Report a
vulnerability**. The report is visible only to you and the maintainers, and the fix is
discussed there before anything is made public.

A useful report says which release or commit and game build you used, what an attacker needs (a
file you open, access to your machine, access to your network), what they can do with it, and
how to reproduce it.

Some behaviour is documented and intended, such as a development tool that listens on the
network without authentication. That is not a vulnerability on its own. It is one if it
happens when the documentation says it does not, or reaches further than the documentation
says.

**In scope here:** `SimDLL.dll`, which runs native code inside the game, including how it reads
saves and the messages the game sends it; and KProfiler's HTTP control listener, which listens
on `127.0.0.1` only, and only when a caller starts it. The offline tools in `driver/` and the
shim in `shim/` are development tools; a problem in them is in scope if a corpus or a save they
are pointed at can do more than make them fail.

## Pull requests are not accepted

This repository is generated from the development repository. A commit made here would be
replaced by the next update, so pull requests cannot be merged.

If you have a fix, describe it in an issue, with a patch or a snippet if it helps. Fixes are
ported by hand, and the changelog entry for the fix links the issue.

## Licensing

A patch or snippet posted in an issue is taken as offered under this repository's license,
the Mozilla Public License 2.0.
