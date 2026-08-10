# Contributing to Zdisplay

Thanks for your interest. This document covers how to build, what the codebase
expects from a change, and how to submit one.

## Building

All you need is a C++17 compiler for Windows (MinGW-w64). If you have none, the
script downloads a portable one itself — no installation, no administrator
rights:

```bash
build.bat
```

With `make` and the toolchain already on your `PATH`:

```bash
make check
```

`make check` builds the application **and** runs the suite. It is the target to
use before opening a pull request — plain `make` does not run the tests.

## Tests

```bash
build.bat test
```

The suite needs no hardware: it runs the same on any machine, including CI with
no display, no discrete GPU and no external monitor. Every behavior change needs
a test.

If you are fixing a defect, write the failing test first. The regressions
section of the suite exists so that a fixed defect stays fixed.

## What the codebase expects

Zdisplay drives the user's entire screen. If an adjustment goes wrong, the
person may not be able to see well enough to undo it. The rules below follow
from that, and are not negotiable:

- **The light floor is absolute.** No input path — a hand-edited file, the
  command line, an imported profile — may produce less than 8% estimated
  luminance. Every input is sanitized on read.
- **Nothing throws.** The binary is built with `-fno-exceptions`. Do not use
  `std::sto*` or `.at()`. Errors are return values.
- **Every backend is optional.** The program must run on a machine with no
  discrete GPU, no external monitor and no DDC/CI. A missing backend bows out
  silently; it is never a prerequisite.
- **DLLs always come from System32.** Runtime loading uses
  `LOAD_LIBRARY_SEARCH_SYSTEM32`. The program is portable and often runs from
  Downloads or a USB stick.
- **DDC/CI writes to the monitor's EEPROM.** Commands go through the existing
  queue, with its minimum interval and per-minute ceiling. Never send a command
  straight to the panel.
- **"Detected" is not "works".** A backend that accepts a command and reports
  success without changing anything must be reported as unavailable, not as
  active.

## Style

- C++17, pure Win32. No external dependencies, no runtime, no framework.
- Code comments in **English**. UI text and log messages in **Portuguese** —
  that is the language of the program's users today (see *Translations* below).
- Comment the **why**, not the what. If a comment only restates the code, it
  should not exist. A hidden constraint, a subtle invariant, or surprising
  Windows API behavior earns a line; nothing else does.
- Write in the present tense and impersonally. A comment is not a development
  diary: do not narrate past defects or compare the code to other programs.
- Build flags live in `flags.mk`, which both the `Makefile` and `build.bat`
  read. Never duplicate a flag in either one.
- The version number lives in `src/version.h`, its single source.

## Pull requests

1. Open an issue before a large change, so the direction is agreed before the
   work is done.
2. One subject per pull request. Refactoring and bug fixes go in separate ones.
3. `make check` must pass. CI runs the same target on Windows.
4. Describe the observable behavior that changed and how to test it by hand,
   where an automated test cannot reach — which is common in UI and hardware
   code.
5. Add an entry under `## [Unreleased]` in [CHANGELOG.md](CHANGELOG.md) when the
   change is user-visible.

## Versioning

[Semantic versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.

- **PATCH** — a fix that changes no setting, no file format and no command line.
- **MINOR** — a new capability, a new backend, a new field in the configuration.
  A configuration written by an older MINOR must still load.
- **MAJOR** — a change that breaks an existing setup: a removed command-line
  flag, a configuration key that no longer migrates, a dropped Windows version.

`src/version.h` is the single source. `ZDISPLAY_VERSION_STR` is the version as
people see it; `ZDISPLAY_VERSION_COMMA` carries a fourth field only because the
Windows VERSIONINFO resource requires four, and it stays zero.

The release tag is `v` followed by that string exactly. CI refuses to publish a
release whose tag and header disagree, so the two cannot drift.

## Releasing

Releases are built by CI, not on a developer machine, so the published SHA-256
belongs to an artifact traceable to a public build log.

1. Bump `ZDISPLAY_VERSION_*` in `src/version.h` and move the `Unreleased`
   section of the changelog under the new version, with the date.
2. Tag the commit `vMAJOR.MINOR.PATCH` and push the tag.
3. The release workflow verifies that the tag matches `src/version.h`, runs the
   suite, checks DEP/ASLR on the binary, builds the installer and the portable
   archive, signs a provenance attestation for each artifact, and attaches them
   with `SHA256SUMS.txt`.

Anyone can then check that a download came from that workflow and that commit:

```bash
gh attestation verify zdisplay-1.0.0-x64.exe --repo zuunypro/zdisplay
```

## Code of Conduct

Participation in this project is governed by the
[Code of Conduct](CODE_OF_CONDUCT.md).

## Reporting a problem

Use the issue templates. For a hardware problem — a monitor that does not
respond, a backend that does not appear — attach the output of:

```bash
zdisplay.exe --diag
```

It lists, per monitor, which backend handles what, and the reason when a control
is unavailable. That information resolves most such reports.

## Security vulnerabilities

**Do not open a public issue.** See [SECURITY.md](SECURITY.md) for the private
channel and for the security boundary the project commits to defending.

## Translations

The program's interface and messages are currently in Portuguese. Translations
into other languages are planned, and the goal is to ship Zdisplay in every
language its users speak. If you would like to help translate it, open an issue
— that contribution is as welcome as any code change.

## License

By contributing, you agree to license your contribution under
[GPL-3.0-or-later](LICENSE), the same license as the project.
