# Zdisplay

[![CI](https://github.com/zuunypro/zdisplay/actions/workflows/ci.yml/badge.svg)](https://github.com/zuunypro/zdisplay/actions/workflows/ci.yml)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/zuunypro/zdisplay)](https://github.com/zuunypro/zdisplay/releases/latest)
[![Windows 10/11](https://img.shields.io/badge/Windows-10%20%7C%2011%20(x64)-0078d4)](https://github.com/zuunypro/zdisplay/releases/latest)

**Brightness, contrast, saturation, digital vibrance, gamma, color temperature,
hue, blue light reduction and black-equalizer shadow detail — for Windows, in
one tray app.** Works on any GPU, with or without DDC/CI, on HDR displays, and
inside exclusive-fullscreen games. Profiles switch themselves by foreground
application or time of day, and everything is scriptable from the command line.

Written in **pure C++ with Win32** — no framework, no runtime, no dependencies.
A 1.6 MB executable using **3.6 MB of RAM and 0% CPU at idle**. The zero is
literal: there is no polling loop anywhere. The program sleeps, and Windows
wakes it through `SetWinEventHook` when the foreground window changes.

**[Download the latest release](https://github.com/zuunypro/zdisplay/releases/latest)**
· [project page](https://zuuny.vercel.app/programas/zdisplay)

> The user interface is currently in Portuguese. Translations are planned — see
> [Other languages](#other-languages).

---

## The part no other tool does

**A black equalizer, in software, on any monitor.**

Gaming monitors sell this as *Black eQualizer*, *Shadow Boost* or *Dark Boost*,
and it only exists in the panel's own menu — if you paid for a panel that has
it. It raises near-black detail so you can see into the unlit corner of a map or
the dim room in a film.

Every implementation has the same complaint: push it up and the shadows flatten
into one grey, and the picture looks washed out. That happens because raising the
black floor also flattens the slope near black, so neighbouring dark tones
collapse into the same value.

Zdisplay splits the job into two sliders. **Shadows** raises the floor;
**Clarity** restores the slope the lift flattens. Measured across the 32 darkest
tones, how many stay distinct from one another:

| Setting | Distinct tones | Tone 128 (midtone) | Tone 255 (white) |
|---|---|---|---|
| neutral | 32 / 32 | 128 | 255 |
| Shadows 100, no Clarity | 20 / 32 | — | — |
| **Shadows 78 + Clarity 65** | **27 / 32** | **129** | **255** |

The midtone moves by one point and white does not move at all. And because it
goes through the gamma ramp rather than an overlay, it works **inside
exclusive-fullscreen games**, with no frame cost and without showing up in
screenshots.

[Full explanation, with the curve and its limits →](#shadow-detail-black-equalizer)

---

## Why it exists

Getting all of this today means running several programs at once — and they
fight each other over the same Windows gamma ramp, where the last writer wins:

| Program | Covers | Stops at |
|---|---|---|
| vibranceGUI | NVIDIA vibrance per game | brightness, contrast, gamma, Intel |
| LightBulb / f.lux | color temperature | saturation, monitor hardware |
| Gammy | adaptive brightness | saturation, per-app profiles |
| Twinkle Tray | DDC/CI brightness (excellent, but Electron: 100–250 MB of RAM) | color |
| Monitorian | lightweight DDC/CI brightness | color |
| DimScreen | dimming below the panel minimum | everything else |

Zdisplay brings the seven functions into a single program, resolves the
conflicts between them, and picks the best available path on each machine by
itself.

## Works on any PC

At startup Zdisplay detects what the machine supports and assembles a backend
stack with automatic fallback. Nothing is required: if the vendor GPU is not
there, the universal path takes over.

| Backend | Controls | Works on | Limits |
|---|---|---|---|
| **Gamma ramp (GDI)** | brightness, contrast, gamma, temperature, white balance, blue light reduction | **any GPU**, per monitor | works even in exclusive-fullscreen games; Windows restricts the range (see below) |
| **Color matrix (Magnification API)** | **saturation**, hue, inversion | **any PC, integrated graphics included** | global effect (not per monitor); does not reach exclusive fullscreen |
| **NVIDIA (NVAPI)** | vibrance (DVC), hue | NVIDIA only | loaded at runtime |
| **AMD (ADL)** | saturation, hue | AMD only | loaded at runtime |
| **DDC/CI** | brightness, contrast and **RGB gain** in the monitor hardware, plus input, color preset and power | compatible external monitors | slow; queued with a minimum interval to spare the EEPROM |
| **SDR white level** | **brightness on an HDR-enabled display**, where the ramp does not apply | Windows 10 1803+, per monitor | governs SDR content; HDR video and games keep their own brightness |
| **Backlight (WMI)** | physical brightness of the internal panel | laptops and all-in-ones | uses the steps the panel declares; matches the WMI instance to the right monitor |
| **Overlay** | dimming below the panel minimum | any PC | washes out contrast; appears in screenshots |

See what your machine supports with `zdisplay.exe --diag` or in the
**Diagnostics** tab (*Diagnóstico*).

The tab lists, **for each monitor**, what handles what — and, when a control
does not work there, why. It answers the question that actually comes up:
*"why does this slider do nothing on this screen?"*

```
AOC FTV (DISPLAY1)
    brilho, contraste, gamma, temperatura, sombras: rampa de gamma
    saturação, vibrance, matiz: AMD (ADL) + matriz de cor
    brilho físico: indisponível (este monitor não respondeu a DDC/CI)
```

That output is the program's own, so it is in Portuguese today. It reads:
brightness, contrast, gamma, temperature and shadows are handled by the gamma
ramp; saturation, vibrance and hue by AMD (ADL) plus the color matrix; hardware
brightness is unavailable, because this monitor did not answer DDC/CI.

### Generic, unbranded monitors with poor firmware

The Windows high-level API (`GetMonitorBrightness` / `SetMonitorBrightness`)
validates the monitor's capability string **before** talking to it. Cheap panels
often report that string truncated, malformed or missing — and the API then
refuses, even when the monitor would have obeyed. Zdisplay tries the high-level
path first and, when it refuses, **speaks VCP directly** to the panel. A monitor
that does not respond even then is shown as "unsupported" rather than vanishing
from the list.

Hardware RGB gain (VCP `0x16` / `0x18` / `0x1A`) is probed by reading the
register itself, not by asking for capabilities. It becomes the color
temperature path when the gamma ramp does not apply — which is the case on an
**HDR-enabled** display, where `SetDeviceGammaRamp` returns success without
changing a single pixel.

### Monitor identity

Each monitor's key comes from its **EDID** (manufacturer + model + serial
number), not from the device path. The path carries the number of the port the
cable is plugged into, so moving to a different output on the card would make
that monitor's own settings disappear. Configurations written in the old format
are migrated automatically.

The EDID also provides the panel's real name instead of "Generic PnP Monitor",
and the gamut coordinates — which is what explains a cheap wide-gamut monitor
oversaturating without anything being wrong in Zdisplay.

### Two graphics adapters in one machine

On a hybrid laptop the display is usually driven by the Intel adapter even when
an NVIDIA one is present. Zdisplay determines which adapter drives each display
(`DISPLAYCONFIG_ADAPTER_NAME`) and uses the vendor path only where it actually
applies. Otherwise the vendor API accepts the command and reports success
without changing anything — a silent failure, the worst kind.

Intel does not expose driver-level vibrance the way NVIDIA and AMD do; on those
machines saturation goes through the universal color matrix, and the diagnostics
say so instead of letting the slider look broken.

---

## Blue light reduction

A slider in the **Settings** tab (*Ajustes*), stored in each profile like any
other adjustment. It exists separately from color temperature on purpose:
temperature shifts the whole white point, so someone who only wants to cut blue
ends up choosing a Kelvin value that also moves red — and then cannot say how
much blue is left. Here the slider's number answers *how much blue am I cutting*
directly.

It cuts blue and, more gently, green; red does not move. At maximum, 15% of blue
remains — zeroing the channel would erase every distinction between the blue
tones in the image, and the light floor would have to undo the adjustment
immediately afterwards.

The slider counts toward the light floor like every other field: no combination
of blue reduction, brightness and temperature can break through it, and that is
tested.

It is the manual, per-profile version of what the **Vision** tab (*Visão*) does
automatically by the clock — the two coexist, and the vision layer never undoes
what the profile asked for.

---

## The Vision tab — eye comfort, in one switch

One switch and four numbers. The screen warms up on its own as the sun sets and
returns to normal in the morning, **on top of any profile** — no need to create
a profile or a schedule rule for it.

| Field | What it does |
|---|---|
| **Day temperature** | 6500 K is neutral white. |
| **Night temperature** | 3400 K is incandescent-lamp color. The lower it is, the less blue reaches your eyes. |
| **Night brightness** | % of what the profile asks for. It matters as much as color: what tires the eyes is the screen being much brighter than the room. |
| **Transition smoothness** | Minutes. The change happens gradually **around** the time, half before and half after. An hour is already imperceptible. |
| **Night / day begins** | A clock time (`22:00`) or the sun itself: `por`, `nascer`, `por-30`, `nascer+45`. |
| **Eye break** | Every N minutes, a discreet tray reminder to look at something about 6 metres away for 20 seconds. |

The layer **never works against what you chose**: if the active profile already
asks for something warmer or darker than the night target, the profile wins. It
only pulls toward comfort.

The *preview* buttons show the result while you hold them — choosing a night
temperature without seeing it would be guesswork.

**On what actually helps:** reducing blue at night has evidence for *sleep*, not
for eye strain. What the clinical literature actually recommends against screen
fatigue is the 20-20-20 break — which is why it is here, and why the night
brightness field exists: the difference between the screen and the room matters
more than the color.

---

## Shadow detail (black equalizer)

Two sliders in the **Settings** tab for the problem of seeing what is in the
dark — the unlit corner of the map, the dim room in a film — **without washing
out the rest of the image**. It is what gaming monitors sell as *Black
eQualizer* or *Shadow Boost*, done in software and on any monitor.

| Slider | What it does |
|---|---|
| **Shadows** (0–100) | raises the black floor with a weight that **dies out before the midtones** |
| **Clarity** (0–100) | pushes near-black tones apart from each other, restoring the detail the lift would flatten |

**Why two and not one.** Raising brightness or gamma lightens the entire image:
the dark appears, but the midtones lose body and the highlights blow out.
Restricting the effect to the low end of the curve solves that — except that
raising the floor flattens the slope near black, and neighbouring tones collapse
into the same value. The detail disappears exactly where you went looking for
it. The second slider restores that slope.

Measuring across the 32 darkest tones, how many remain **distinct** from one
another:

| Setting | Distinct tones | Tone 0 becomes |
|---|---|---|
| neutral | 32 / 32 | 0 |
| Shadows 100 alone | 20 / 32 | 40 |
| Shadows 100 + Clarity 100 | **29 / 32** | 40 |
| Shadows 78 + Clarity 65 | 27 / 32 | 31 |

And what happens to the rest of the image at **Shadows 78 + Clarity 65**:

| Input tone | 4 | 10 | 40 | 80 | 128 | 180 | 220 | 255 |
|---|---|---|---|---|---|---|---|---|
| Output tone | 35 | **41** | 64 | 89 | 129 | 180 | 220 | 255 |

Tone 10 — invisible in practice — becomes 41. Tone 128 moves by one point. From
180 upward **nothing changes**: whites, sky and interface stay exactly where
they were.

Because all of this goes through the **gamma ramp**, it also applies inside
exclusive-fullscreen games, with no overlay, no CPU cost, and without appearing
in screenshots.

The built-in **Competitivo** profile ships with Shadows 78 / Clarity 65, and the
**Jogo** profile gets a light dose (40 / 30).

### What these sliders are not

- **They are not edge sharpening.** Real sharpening compares each pixel with its
  neighbours, and the gamma ramp is a 256-entry table that cannot see any
  neighbourhood at all — only the tone. Doing that would require capturing and
  reprocessing the screen frame by frame, which costs GPU time, adds latency and
  does not work in exclusive fullscreen. What **Clarity** does is low-tone
  contrast, which is what actually brings dark detail back.
- **Dark colors lose a little saturation.** The table is per channel and treats
  each one without knowing the others, so adding the same floor to R, G and B
  brings the three closer together — and closer means washed out. If it bothers
  you, compensate with +5 to +10 on **Saturation**.
- **The strongest setting still merges 3 tones out of 32**, because of the
  8 bits in the signal. That is the price of stretching the low end of the
  curve; it cannot be reduced to zero in a tone table.

---

## Building

All you need is a C++ compiler (MinGW-w64). If you have none, the script
downloads a portable one itself — no installation, no administrator rights.

```bash
build.bat
```

Or, with `make` on your `PATH`:

```bash
make
```

The icon is drawn by the program itself (`zdisplay.exe --make-icon`) — there is
no dependency on any graphics tool.

### Building the installer

```bash
build.bat setup
```

The result is `zdisplay-setup.exe`, a self-contained graphical installer. It
installs per user in `%LOCALAPPDATA%\Programs\Zdisplay`, creates the Start menu
shortcut, registers Zdisplay in **Installed apps** and includes the uninstaller.
It never asks for administrator rights. The embedded executable is validated by
CRC-32, SHA-256 and PE x64 structure before a single file is written.

## Using it

```bash
zdisplay.exe
```

Double-click the tray icon to open the settings; middle-click pauses and
restores the screen immediately.

### Command line

With Zdisplay already running, any invocation becomes a command to the running
instance — useful for scripts, Stream Deck, AutoHotkey or Windows shortcuts.

```bash
zdisplay.exe --profile "Jogo"      # activate a profile (built-in name: "Game")
zdisplay.exe --brightness 70       # software brightness (10..150)
zdisplay.exe --saturation 130      # saturation (0..200)
zdisplay.exe --vibrance 60         # GPU vibrance (0..100)
zdisplay.exe --temperature 3400    # color temperature in Kelvin
zdisplay.exe --shadows 78          # raise only the dark tones (0..100)
zdisplay.exe --clarity 65          # shadow detail (0..100)
zdisplay.exe --hwbrightness 40     # monitor hardware brightness (DDC/CI)
zdisplay.exe --dim 25              # extra dimming via overlay
zdisplay.exe --auto                # back to automatic mode
zdisplay.exe --toggle              # pause / resume
zdisplay.exe --reset               # return the screen to its original state
zdisplay.exe --panic               # EMERGENCY: restore the screen and pause
zdisplay.exe --status              # print the current state
zdisplay.exe --diag                # print the detected backends
zdisplay.exe --list                # list the profiles
zdisplay.exe --aba 5               # open on a tab: 0 Settings, 1 Vision,
                                   #   2 Profiles, 3 Automation, 4 System,
                                   #   5 Diagnostics
zdisplay.exe --quit
```

The commands that set a value (`--brightness`, `--saturation`, …) change the
profile's **global** value. Monitors with their own override keep what you
configured for them — the response tells you how many were left out. To change a
specific monitor, use the window.

Called from a terminal, it writes the response to the terminal; outside one, it
shows a dialog. `zdisplay.exe --help` lists everything.

### Global hotkeys (defaults)

| Hotkey | Action |
|---|---|
| `Ctrl+Alt+↑` / `↓` | brightness |
| `Ctrl+Alt+→` / `←` | saturation |
| `Ctrl+Alt+K` | pause / resume |
| `Ctrl+Alt+P` | open the window |
| `Ctrl+Alt+Shift+K` | **emergency**: restore the screen and pause |

Configurable in the **System** tab (*Sistema*), and each profile can have its
own.

### Profiles and automation

A profile stores every adjustment, with optional **per-monitor** overrides. Six
ship built in: Padrão, Jogo, Competitivo, Noite, Filme and Leitura (Default,
Game, Competitive, Night, Film, Reading).

Profile selection follows this priority:

1. a profile pinned by hand (tray, window or hotkey);
2. a **per-application rule** — Zdisplay uses `SetWinEventHook` to know which
   program is in the foreground, with no polling loop, so the idle cost is zero;
3. a **schedule rule** (ranges may cross midnight);
4. the default profile.

A schedule rule's *start* and *end* fields accept a clock time (`22:00`) or the
**sun itself**: `por` (sunset), `nascer` (sunrise), and with an offset such as
`por-30` or `nascer+45`. Sunset moves by more than two hours over the year, so a
fixed range is wrong for half the months. For this to work, fill in *Latitude*
and *Longitude* in the same tab — without a location, a solar rule simply does
not match, rather than switching profiles at the time of a place you are not in.

Within each level, the higher **priority** wins — for both application and
schedule rules — so that two overlapping ranges have an explicit tiebreak
instead of depending on line order in the file.

**You do not need to know the executable's name.** The *Process* field, in the
**Automation** tab (*Automação*), is a list of the programs you currently have
open — the same list as Alt+Tab, re-enumerated every time you open the menu.
Pick one and you are done. The field still accepts typed text, for programs that
are not running at the moment and for wildcards: `cs*` matches `cs2` and `csgo`.
The **use the foreground program** button is still there for the fullscreen-game
case, which disappears from the list while you are in the Zdisplay window.

Transitions are smoothed frame by frame. Slow hardware commands (DDC/CI and
backlight) are sent only at the final value, never during the transition.

**Keyboard brightness keys.** On a docked laptop, the brightness keys only move
the internal panel and the two screens end up mismatched. With *brightness keys
also apply to external monitors* enabled (**System** tab), Zdisplay notices the
change and carries the same value to external monitors over DDC/CI. A profile
already managing hardware brightness still wins: in that case mirroring stays
out of the way, so the two do not fight.

### Configuration

A readable, hand-editable INI file, written atomically:

```
%APPDATA%\Zdisplay\zdisplay.ini
```

**Portable mode:** create an empty file named `zdisplay-portable.txt` next to
the executable and everything is written to that same folder — nothing in the
registry (unless you enable autostart), nothing in `%APPDATA%`.

---

## Safeguards

The program drives the entire screen. If an adjustment goes wrong, the user may
not be able to see well enough to undo it — so the safeguards below exist to
make sure that never happens.

**Reset restores *your* screen, not a "neutral" one.** At startup Zdisplay reads
and stores in `baseline.dat` each monitor's gamma ramp, its physical brightness
and contrast (DDC/CI), the laptop backlight, and the vibrance already set in the
GPU panel. Reset restores exactly those values. If the monitor has an ICC
calibration, it is **preserved**: adjustments are applied *on top of* the
calibration, with interpolation to avoid banding.

**Recovery after a crash.** While running, a `session.lock` file exists. If it
is still there at the next startup, the previous run did not end cleanly — and
Zdisplay restores the screen to the stored state before doing anything else. Not
even pulling the plug leaves the screen stuck on an adjustment.

**Confirmation with automatic revert.** When the adjustments make the screen too
dark (less than 20% estimated light), a window appears with a 15-second
countdown. Doing nothing undoes the adjustment — the same design Windows uses
when changing resolution, and it works precisely in the case where the user
cannot see the button. If Zdisplay starts up already in a dark state, the revert
point becomes neutral.

**Emergency hotkey: `Ctrl+Alt+Shift+K`.** Restores the screen and pauses, from
anywhere in Windows. Clearing that field in the settings does not disable it —
the default comes back. Also available as `zdisplay.exe --panic` and in the tray
menu.

**Absolute light floor.** No input path — a hand-edited file, the command line,
a profile imported from another machine — can produce less than 8% light.
Out-of-range values, `NaN` and infinity are corrected on read.

The remaining light is **measured on the curve that goes to the screen**, not
estimated by a separate formula: brightness, contrast, gamma, temperature, white
balance and shadow detail all enter the calculation, with Rec.709 weights. That
matters because each of them genuinely darkens the image — gamma 0.30 with the
gains at 50 and temperature at 1500 K leaves the screen nearly black without the
brightness slider ever leaving 100%. When the combination falls below the floor,
Zdisplay backs off in the order that costs least: first the overlay, then
hardware brightness, software brightness, the gains, temperature, gamma and
finally contrast.

**The configuration is backed up.** Atomic writes, a `zdisplay.ini.bak` of the
previous version, and tolerant reading: a corrupted file falls back to the
backup, and a file with nothing usable in it is kept as `.invalido` instead of
being silently overwritten.

**The monitor's EEPROM is spared.** DDC/CI commands go through a queue with a
minimum interval of 140 ms, are coalesced when repeated, are never sent during
transitions, and have a hard ceiling of 40 per minute per monitor.

**Every system change is reversible.** The only point that asks for
administrator rights is unlocking the gamma range, and the same button undoes
it. Autostart uses `HKCU\...\Run`, which disappears along with the program.

## Tests

```bash
build.bat test
```

Or, to build the program **and** run the suite in one invocation:

```bash
make check
```

404 tests that do not depend on hardware — they run the same on any machine.
They cover the color math (5,400 ramp combinations verifying that it never
decreases nor blanks the screen), the 441 combinations of the shadow curve, the
safety limits, the application and schedule rules, the configuration round trip,
ten deliberately corrupted configuration files, the recording of the original
state, and eight simulated PC configurations (NVIDIA, AMD, Intel, laptop,
virtual machine, four monitors, and a machine with no backend available at all).

A **regressions** section pins down, one by one, the defects the audit found:
the light floor broken through by temperature and gains, the `NaN` that crossed
`Clamp`, the imported profile that was not sanitized, the discontinuity in the
temperature curve at 6600 K, the shadows swallowing the image at low brightness,
and the truncated baseline leaving half a state behind.

What the hardware reports about itself is also tested without any hardware: the
**EDID** is assembled byte by byte in the test (header, checksum, manufacturer
packed into 5 bits, sRGB and DCI-P3 primaries) and the parser must reject a
truncated, zeroed or bad-checksum block — because accepting one would invent a
"serial number", and the per-monitor key would then point at the wrong panel.
The **DDC/CI capability string** is tested against a real response, including
the case that misleads most: the numbers inside `14(01 05 06)` are the values
accepted by that code, not codes themselves — and it is precisely that list that
decides which inputs the program offers, so it is also tested against firmware
that strips all whitespace and against non-hexadecimal garbage, which arrives
over I2C and is not trustworthy data.

Converting a **WMI instance name** to the canonical device path has its own
section, because that is what matches the panel WMI commands to the monitor the
rest of the program knows: two built-in panels of the same model must stay
distinct, or brightness goes to the wrong screen. **Per-monitor-model rules** are
tested round-tripping through the file, and a rule with a typo must be rejected
rather than becoming an empty rule that silently does nothing.

**Sunrise and sunset** are checked against the solstices in São Paulo and London
(within 5 to 10 minutes), plus the invariants that catch a sign flip: December
is the long day in the southern hemisphere and the short one in the north, at
the equator the day lasts a little over 12 h all year, and above the polar circle
the function must report that there is no sunrise or sunset rather than
returning a number.

## The Windows gamma limit

Without a registry change, Windows **refuses** gamma ramps that deviate too far
from linear — and refuses silently. That is why programs of this kind sometimes
"do nothing" in strong combinations (low brightness + warm temperature).

Zdisplay does not pretend it worked: when Windows refuses, it searches for the
largest fraction of the effect the system accepts, applies that fraction and
says so in the status bar and in the diagnostics ("Windows limited the effect to
X%"). It also periodically tries to return to the full effect, in case you
unlock the range.

This weighs especially on **shadow detail**: raising the black floor is by
definition a large deviation from linear, so it is the adjustment that most
often hits the Windows ceiling. It keeps working at the accepted fraction — just
not at full strength. If Shadows seems weak, this is almost always why; check the
warning in the diagnostics before blaming the slider.

To unlock the full range: **System** tab → *unlock the full gamma range*. This
writes `GdiIcmGammaRange=256` to
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ICM`, requires one run as
administrator, and only takes effect after restarting the Windows session.

## Other honest caveats

- **Windows Night light** competes for the same ramp. Zdisplay detects it and
  warns; the "reapply adjustments every N seconds" option (System tab)
  reasserts the values when another program overwrites them.
- **Exclusive-fullscreen games**: the gamma ramp and the vendor APIs keep
  working; the universal matrix and the overlay do not. Use borderless windowed
  mode if you need universal saturation inside the game.
- **HDR enabled** makes Windows ignore the gamma ramp — it accepts the write and
  changes nothing. On those displays brightness goes through the **SDR white
  level**, which is the same thing the Windows "SDR content brightness" slider
  moves: it works per display, with no I2C and no EEPROM writes, and it applies
  in exclusive-fullscreen games.

  | Arrangement | What takes over |
  |---|---|
  | Any display in HDR | **brightness: SDR white level**, anchored to the value the display already had |
  | All displays in HDR | contrast and temperature go through the **color matrix** |
  | HDR on some displays | contrast and temperature have no effect on the HDR display |

  In the mixed arrangement the matrix cannot take over contrast and temperature:
  its effect covers the entire desktop, and the non-HDR displays — which already
  received the ramp — would get the adjustment twice. Gamma and shadow detail are
  curves with no equivalent, so they remain without effect on an HDR display.

  Detection uses the Windows 11 24H2 **active mode** query, not just the old
  "advanced color" bit. The difference matters: with Auto Color Management on,
  the old bit reads true **with the display in SDR**, and anything trusting it
  disables gamma and shadows for no reason on a machine where they work.
- **DDC/CI writes to the monitor's EEPROM.** Zdisplay rate-limits, coalesces
  requests and never sends commands during transitions — and deliberately does
  **not** restore hardware brightness on exit, because that value is yours.
- **Monitors that do not follow the standard** have their own table. There are
  panels that report brightness on a different register (and accept the command
  on 0x10 without changing anything) and firmware that crashes the display driver
  on receiving DDC/CI. What is already known ships built in; you add the rest in
  the configuration file, per model:

  ```ini
  [modelo:FUS087C]
  regra=brilho-vcp:6B
  ```

  It accepts `bloquear` (block), `sem-capacidades` (no capabilities) and
  `brilho-vcp:XX` (brightness VCP register), and your rule outranks the built-in
  one — so you can undo a factory entry that gets in the way of your hardware.
  The identifier is the EDID one (three manufacturer letters + product code),
  visible in the **Diagnostics** tab.
- **"Detected" is not "works".** The *test the monitor* button, in the
  Diagnostics tab, changes brightness by one step, reads it back, checks it and
  restores the previous value. That is what separates a monitor that obeys from
  one that accepts the command, reports success and changes nothing — common
  enough to deserve proof, and impossible to diagnose by eye.

## Security

No network, no telemetry, no elevation. The only point that asks for
administrator rights is the button that unlocks the gamma range, and it is
optional and explicitly confirmed. Autostart uses `HKCU\...\Run`, trivial to
undo.

The system DLLs Zdisplay loads at runtime (`nvapi64`, `atiadlxx`, `dxva2`,
`magnification`) always come from System32, via `SetDefaultDllDirectories` plus
`LOAD_LIBRARY_SEARCH_SYSTEM32`. Without that, the executable's own directory
precedes the system one in the search order — and in a portable program, which
is often run from Downloads or a USB stick, a single file with the right name
next to it would be enough to become code execution.

The release build fails if the executable or the installer are not marked with
DEP, ASLR and high-entropy ASLR. At runtime, both also refuse remote and
low-integrity images. The uninstaller removes only the files the installer
created; it never recursively deletes a folder chosen by the user.

The single instance and the command channel are **per Windows session**, not per
machine: on a PC with two users logged in, each has their own Zdisplay and
neither interferes with the other.

Full policy and reporting channel: [SECURITY.md](SECURITY.md).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for building, running the suite, and the
rules the codebase does not bend on — the absolute light floor, the
exception-free binary, every backend being optional, and the ban on treating
"detected" as "works". Participation is governed by the
[Code of Conduct](CODE_OF_CONDUCT.md).

Release history is in the [changelog](CHANGELOG.md).

Security vulnerabilities do not go in public issues: see
[SECURITY.md](SECURITY.md).

## Layout

```
src/
  version.h           version number, used by the .rc and by the code
  ui_dpi.h            per-DPI scaling, shared between the windows
  ui_theme.cpp/.h     dark theme: palette, window frame and owner-drawing
                      for the controls that ignore color messages
  common.*            utilities, log, paths, runtime DLL loading
  core.*              adjustments, profiles, INI, color math, monitors
  backends.h          common interface and capabilities
  backends_display.*  gamma ramp, universal matrix, overlay
  backends_vendor.*   NVAPI (NVIDIA) and ADL (AMD)
  backends_hw.*       DDC/CI and WMI backlight
  engine.*            profile resolution, transitions, watchdog, fallbacks
  services.*          global hotkeys, foreground app, named pipe, autostart
  ui_*.cpp            tray, settings window and events
  icon.cpp            the icon, drawn in code
  main.cpp            entry point, single instance, CLI
```

Code comments are in **English**; interface text and log messages are in
**Portuguese** — the language of the program's users today.

## Other languages

The program's interface and messages are currently in Portuguese. Translations
are planned, and the goal is to ship Zdisplay in every language its users speak.
If you would like to help translate it, [open an
issue](https://github.com/zuunypro/zdisplay/issues/new/choose) — that
contribution is as welcome as any code change.

## License

[GPL-3.0-or-later](LICENSE). You may use, study, modify and redistribute it. If
you distribute a modified version, it must come with its source code and under
the same license.
