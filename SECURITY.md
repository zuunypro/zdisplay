# Zdisplay security policy

## Reporting a vulnerability

**Do not open a public issue for a security vulnerability.** Use the
repository's private reporting channel (the *Security* tab → *Report a
vulnerability*), visible only to you and the maintainers.

A good report includes: the Zdisplay version (`--status`), the Windows version,
what happens, and how to reproduce it. Attach a proof of concept if you have
one — it stays private while the report is open.

Expect a response within 7 days. Fixes are published before disclosure.

## What counts as a vulnerability

Zdisplay runs without administrator privileges (`asInvoker`), inside the user's
session. This is the boundary it commits to defending:

**Counts as a vulnerability:**

- Anything that lets a process belonging to **another user** on the machine
  read, alter or influence one user's Zdisplay — through the command channel,
  the configuration file, or any other path.
- Code execution inside the Zdisplay process from data it reads but does not
  control: `zdisplay.ini`, `baseline.dat`, an imported profile, or the EDID and
  MCCS capability string coming from the monitor.
- Loading a DLL from any directory other than `System32`.
- Writing to a file outside the configuration directory via a caller-controlled
  path.

**Does not count** (by design, not by oversight):

- A process belonging to the **same user** sending commands over the channel.
  That is its purpose: command line, Stream Deck, AutoHotkey. Anything already
  running as you can do everything you can do.
- A process belonging to the same user editing `zdisplay.ini`. Same reason.
- Zdisplay in portable mode reading its configuration from a directory another
  user can write to. If the executable's directory is writable by third
  parties, so is the executable itself — there is nothing left to defend. Use
  the normal installation (`%APPDATA%\Zdisplay`) on a shared machine.

## Protections in place

| Area | Measure |
|---|---|
| DLL loading | `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)` at the start of `wWinMain`, and `LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_SYSTEM32` for every load |
| Command channel (server) | Single instance via `FILE_FLAG_FIRST_PIPE_INSTANCE`, an explicit and protected DACL (owner and SYSTEM only), `PIPE_REJECT_REMOTE_CLIENTS`, and a loud, visible failure if the name is already taken |
| Command channel (client) | `SECURITY_SQOS_PRESENT \| SECURITY_IDENTIFICATION` (the server cannot impersonate the caller) and verification of the server process owner's SID before sending |
| Window messages | `WM_ZDISPLAY_COMMAND` carries an opaque cookie validated against a table, never a pointer |
| Helper process | `--ddc-caps-worker` writes only to `caps-result-*.tmp` inside the configuration directory |
| External data | EDID header and checksum are verified; `baseline.dat` bounds counts and sizes before allocating; no `std::sto*` and no `.at()` anywhere, since the binary is built `-fno-exceptions` |
| Binary | DEP, ASLR, high-entropy ASLR and `-fstack-protector-strong`, all set explicitly in `flags.mk` |
| Process | Terminates on heap corruption and refuses remote and low-integrity images; prefers images from `System32` |
| Installer | Payload capped at 64 MB and validated by CRC-32, SHA-256 and PE x64 structure before anything is written |
| Uninstall | Absolute, canonical path with the final directory required to be `Zdisplay`; removes only known files and preserves anything the user put there |
| Build | Fails if `objdump` does not confirm DEP, ASLR and high-entropy ASLR on both the application and the installer |
| Toolchain | `build.bat` verifies both the SHA-256 **and** the Authenticode signature of w64devkit before extracting, and never runs the self-extractor |

### Known limitations

- **No Control Flow Guard.** GCC/MinGW does not implement CFG. Enabling it
  would require moving the build to MSVC or clang-cl.
- **The binary is not Authenticode-signed.** Until a certificate is available,
  verify your download against the SHA-256 published with the release.

## Verifying your download

The build is reproducible: the PE timestamp is zeroed
(`-Wl,--no-insert-timestamp`) and every flag comes from `flags.mk`. Building
the same version tag with the same w64devkit release must produce a
`zdisplay.exe` whose SHA-256 matches the one published with the release.

```powershell
Get-FileHash .\zdisplay.exe -Algorithm SHA256
```

If it does not match, do not run it — and tell us through the private channel
above.
