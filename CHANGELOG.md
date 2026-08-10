# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Configuration files are now tested in every encoding the loader claims to
  accept: UTF-16 in both byte orders and UTF-8, each carrying a byte-order mark
  and a profile name with a non-ASCII character, so a decoder that drops or
  swaps a byte fails the test instead of silently producing mojibake.

### Fixed

- Decoding a UTF-16 little-endian configuration file read the byte buffer
  through a `wchar_t` pointer. The alignment works out on Windows and no
  failure was observed, but reading an object through an unrelated pointer type
  is undefined behaviour; both byte orders now assemble the code units a byte at
  a time, as the big-endian path already did.

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

[Unreleased]: https://github.com/zuunypro/zdisplay/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/zuunypro/zdisplay/releases/tag/v1.0.0
