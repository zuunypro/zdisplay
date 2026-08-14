# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0] — 2026-08-14

Zdisplay speaks English. The interface, the installer and `--help` are written
in English and translated into Portuguese, following the Windows language on
their own. A performance setting picks how hard the program works, mirrored
displays are adjusted on every panel, and a monitor that answers nothing is
reported as detected rather than left out.

### Added

- Interface language, with **English as the primary language** and Portuguese
  as a translation of it. `idioma=auto` follows the Windows UI language, and
  `pt` or `en` pin it. Every regional variant of Portuguese resolves to
  Portuguese and every other language to English, so a machine set to a
  language this program does not carry gets readable text rather than blank
  captions. English is also the column every other one falls back to, so a
  missing translation shows real text. The table is built into the binary;
  there are no translation files to ship or to validate.
  A message is keyed by its English wording, so a call site reads as the
  sentence it produces and the source stays in the language the repository is
  written in. Under English the key is the answer, so the primary language
  costs no lookup at all.
  Every window is converted: the tray menu, all six tabs, every tooltip, every
  dialog and the dark-screen confirmation. `--help` is translated too, and
  follows the Windows language rather than the configuration file, because
  printing help is not a reason to create a configuration file that does not
  exist yet.
- The installer speaks the same two languages, following the Windows UI
  language. It carries a table of its own rather than the application's,
  because it is a single self-contained executable and would otherwise grow by
  several hundred messages it never says.
- The shipped profiles are named in the interface language on a fresh install.
  Naming happens only at seeding, so an existing configuration keeps the names
  it already has and no rule is left pointing at a profile that was renamed
  underneath it.
- The command line accepts `--tab` and `--gamma-range` alongside the Portuguese
  spellings, which keep working.

### Changed

- The log, the Diagnostics tab and the `--diag` output are in English and are
  not translated. They exist to be read by whoever maintains the program, in a
  problem report, so they say the same thing whatever the reader's language is.
- The saturation engine is written to the configuration as `off` rather than
  `desligado`. The old spelling is still read, so a file from 1.0 loads
  unchanged.
- The backup a factory reset leaves behind is named `.before-reset`, and an
  unreadable configuration is kept as `.invalid`.
- The settings window is 40 px taller, to hold the two rows added to the System
  tab. Two notes on that tab that repeated their own tooltip word for word were
  removed rather than kept twice.
- A performance setting with three levels. *Quality* reasserts every 5 s and
  reaches the largest effect Windows accepts while a slider is still moving;
  *Balanced* is the previous behaviour, unchanged; *Light* reasserts every 30 s
  and switches profile without animation. No level turns a backend off, so
  changing it can never make a control stop working. The values each level
  stands for are written into the configuration file, so what is in effect
  stays readable and hand-editable instead of being implied by a preset.
- Mirrored displays are adjusted and restored on every panel. Windows presents
  a clone pair as one desktop area with two physical monitors behind it, and
  only the first one that answered was being controlled; each panel now keeps
  its own reported ranges, its own original values and its own write budget.
- Monitors that answer no DDC/CI register are reported in the diagnostics as
  detected but mute. Leaving them out of the list was indistinguishable from
  not having detected them at all.
- Monitor arrival and removal are observed directly. `WM_DISPLAYCHANGE` covers
  the plug that moves the desktop layout, but not a panel swapped on a KVM into
  the same resolution, nor a dock that publishes the monitor after the video
  topology has already settled.
- Configuration files are now tested in every encoding the loader claims to
  accept: UTF-16 in both byte orders and UTF-8, each carrying a byte-order mark
  and a profile name with a non-ASCII character, so a decoder that drops or
  swaps a byte fails the test instead of silently producing mojibake.

### Removed

- Dead code: an unused DPI helper in the settings window.

### Fixed

- The restore performed on exit wrote brightness to the standard register
  instead of the panel's own. On models that answer on a private code — two of
  which are in the built-in quirk table — the write was accepted and ignored,
  so the monitor kept the adjusted brightness after Zdisplay had closed.
- A failed brightness write named the standard register in the log rather than
  the one actually used, so the diagnostics reported the wrong code on exactly
  the monitors where the difference matters.
- Decoding a UTF-16 little-endian configuration file read the byte buffer
  through a `wchar_t` pointer. The alignment works out on Windows and no
  failure was observed, but reading an object through an unrelated pointer type
  is undefined behaviour; both byte orders now assemble the code units a byte at
  a time, as the big-endian path already did.
- `make` ended in "command not found" on every POSIX shell, including the three
  the Makefile documents, even when the whole build had succeeded. The warning
  printed when the copy to the project root fails contained an unquoted `;`,
  which the shell read as a command separator and then tried to run the rest of
  the sentence. It went unnoticed because continuous integration builds through
  cmd.exe, where `;` is ordinary text.

## [1.0.0] — 2026-08-10

First public, stable release.

### Added

- **Seven adjustments in one program**: brightness, contrast, saturation and
  vibrance, gamma, color temperature, hue, white balance and blue light
  reduction.
- **Shadow detail (black equalizer)** as two sliders — *Shadows* raises the
  black floor, *Clarity* restores the slope the lift flattens — applied through
  the gamma ramp, so it works inside exclusive-fullscreen games with no frame
  cost.
- **Backend stack with automatic fallback**: GDI gamma ramp, Magnification color
  matrix, NVAPI (NVIDIA), ADL (AMD), DDC/CI, SDR white level for HDR displays,
  WMI backlight, and a dimming overlay. Nothing is required; the program reports
  per monitor which control works and why one does not.
- **Profiles** with optional per-monitor overrides, switching automatically by
  foreground application or schedule, including sunrise- and sunset-relative
  times.
- **Vision tab**: automatic warming and dimming across the day, layered on top
  of any profile, plus a 20-20-20 eye break reminder.
- **Command line and named pipe**, so a second invocation becomes a command to
  the running instance — usable from scripts, Stream Deck and AutoHotkey.
- **Global hotkeys**, configurable, with a per-profile override.
- **Portable mode** via a `zdisplay-portable.txt` marker next to the executable.
- **Self-contained installer** that installs per user without administrator
  rights and validates its embedded payload by CRC-32, SHA-256 and PE x64
  structure before writing anything.

### Safety

- Absolute light floor of 8% estimated luminance that no input path can break
  through, measured on the curve actually sent to the display.
- Confirmation dialog with automatic revert after 15 seconds when adjustments
  make the screen too dark.
- Emergency hotkey `Ctrl+Alt+Shift+K`, also available as `--panic`, which
  restores the screen and pauses.
- Crash recovery through a session lock: an unclean shutdown is detected at the
  next start and the screen is restored before anything else runs.
- The original state of every monitor is captured at startup and restored on
  reset, preserving any ICC calibration instead of overwriting it with a linear
  ramp.
- DDC/CI writes are queued with a minimum interval, coalesced, never sent during
  transitions, and capped per minute, to spare the monitor's EEPROM.

### Security

- All runtime DLLs resolve from `System32` only.
- The command channel is per Windows session, with an explicit protected DACL,
  and refuses remote clients; the client verifies the server's owner SID before
  sending.
- The binary is built with DEP, ASLR, high-entropy ASLR and stack protection,
  all verified at build time.

### Build and distribution

- Releases are built by GitHub Actions using the w64devkit release pinned in
  `flags.mk`, so a published binary has a public build log and reproduces the
  documented build rather than one developer's machine. Each artifact carries a
  signed provenance attestation, alongside `SHA256SUMS.txt` from the same run.
- Three download shapes: an installer, a portable archive that already contains
  the `zdisplay-portable.txt` marker, and the bare executable.
- Continuous integration builds and runs the suite on Windows, verifies the
  binary hardening flags, runs the suite again under AddressSanitizer and
  UndefinedBehaviorSanitizer, and analyses the sources with CodeQL. Workflow
  actions are pinned by commit digest.

[Unreleased]: https://github.com/zuunypro/zdisplay/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/zuunypro/zdisplay/releases/tag/v1.1.0
[1.0.0]: https://github.com/zuunypro/zdisplay/releases/tag/v1.0.0
