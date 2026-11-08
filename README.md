# CRTCast for Windows · v0.4

A Windows app that shows up as an **AirPlay Screen Mirroring** receiver for your iPhone and forwards the
picture and sound to a [MiSTer](https://github.com/MiSTer-devel) running the **Groovy** core
([psakhis/Groovy_MiSTer](https://github.com/psakhis/Groovy_MiSTer)), so the MiSTer drives a real 15 kHz CRT.

It's the Windows counterpart of CRTCast Mac from [Ewkerman/CRTCast](https://github.com/Ewkerman/CRTCast)
(Apple TV and Mac apps), with the same engine design: the same Groovy sender, the same audio scheduling and
lip sync, and the same green phosphor look.

## What it does

- Appears as **CRTCast Windows** in the iPhone's Control Center → Screen Mirroring list (no Bonjour install needed).
- Streams start on their own when the phone mirrors and stop when it disconnects. Brief AirPlay drops
  (under 4 s) don't restart the stream, because a restart makes the CRT re-lock.
- Picture: H.264/H.265 decoded with FFmpeg, scaled to 640×480 letterboxed (a portrait phone becomes a centred
  column), sent as 640×480i, 640×240p or 320×240p.
- Sound: AAC-ELD / AAC-LC / ALAC decoded to exactly 48 kHz, kept in step with the MiSTer's clock by a
  drift-corrected resampler (±0.5 %, 80 ms buffer), with lip sync from the phone's timestamps.
  A **Lowest delay** option shows the newest picture instead (for games).
- A **test pattern** with a beep and a white flash once a second (they should happen together on the CRT).
- Live preview, a status line (late fields, audio buffer, timings), and a system-tray icon with Stop/Resume.

## Using it

1. On the MiSTer, load the Groovy core. In its OSD: **Audio → On**.
2. Start `CRTCastWin.exe`, enter the MiSTer's IP address.
3. When Windows Firewall asks, **allow CRTCastWin** (TCP and UDP). Without that, the iPhone can't see or reach
   the PC, especially on a network marked Public.
4. On the iPhone: Control Center → Screen Mirroring → **CRTCast Windows**.

Settings: resolution (default 640×480i), RGB565 (default off: RGB888 looks best and fits wired Ethernet;
RGB565 needs a third less bandwidth), send audio, audio delay (±150 ms), lowest delay.

**Use wired Ethernet if you can.** The MiSTer has no frame buffer in this mode: it draws each field as it
arrives, so a field that's more than ~16 ms late makes the picture jump. Windows Wi-Fi background scans can
cause exactly that. Watch the **late fields** counter.

Closing the window keeps CRTCast running in the tray. Quit from the tray menu.

## Layout

| Part | Folder | What |
|---|---|---|
| App | `app/` | `CRTCastWin.exe` (C# / WPF, .NET 8): phosphor UI, settings, preview, tray icon. |
| Native core | `core/` | `crtcast_core.dll` (C++17, CMake): Groovy send loop, audio schedule, AirPlay receiver, FFmpeg decode, lip sync, test pattern. Small C API in `core/include/crtcast_core.h`. |
| Test tool | `core/tools/` | `crtcast_test.exe`: command-line test pattern + beep, a connectivity probe, and `--airplay [s]` (receiver only). |
| AirPlay (Windows parts) | `core/airplay/` | UxPlay's own mDNS responder (`uxplay_win/mdnsd`) and its original OpenSSL `crypto.c`/`srp.c` (UxPlay commit 05b0275), plus MSVC compatibility headers. |
| Shared code | `shared/` | Unchanged copies of what the Apple apps use: `Groovy/` (groovymister sender from Groovy_MiSTer `api/` + LZ4) and `AirPlay/` (UxPlay core + `AirPlayBridge`). |
| FFmpeg recipe | `vcpkg-ports/ffmpeg` | vcpkg's FFmpeg port with one line that builds only the decoders CRTCast uses. |

Why a native core with a C# UI: everything timing-critical (one field every 16.7 ms, raster sync, audio
pacing) runs on a native time-critical thread, where .NET garbage collection can't delay a field, and the C
libraries (groovymister, UxPlay, OpenSSL, libplist, FFmpeg) stay in C/C++. WPF makes the UI and tray icon
quick to build.

`groovymister.cpp` is compiled **unchanged**. Upstream's `api/rio.h` is written for MinGW and clashes with
the Windows SDK, so `core/compat/rio.h` stands in for it under MSVC (it just includes `<mswsock.h>`).
`core/src/groovy_rio.cpp` empties groovymister's Registered I/O send-completion queue after each send;
groovymister never does, and the overflowing queue made Windows terminate the process now and then.

## Build

Needs Visual Studio 2019+ (or Build Tools) with C++ x64, CMake, Ninja, the .NET 8 SDK, and
[vcpkg](https://github.com/microsoft/vcpkg) with these static libraries (everything ends up in one DLL):

```powershell
vcpkg install openssl:x64-windows-static libplist:x64-windows-static pthreads:x64-windows-static
vcpkg install "ffmpeg[core,avcodec,swresample]:x64-windows-static" --overlay-ports=vcpkg-ports
```

The trimmed FFmpeg recipe builds only H.264, HEVC, AAC/AAC-ELD and ALAC. The full FFmpeg takes far longer,
and one of its decoders (`sanm.c`) stalls the Visual Studio 2019 optimizer.

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

`build.ps1` uses the first vcpkg (`VCPKG_ROOT`, then `%USERPROFILE%\vcpkg`) that has these installed.
Output: `app\bin\Release\net8.0-windows\win-x64\` (`CRTCastWin.exe`, `crtcast_core.dll`, `crtcast_test.exe`).

Keep the folder path short (Windows' 260-character limit trips up the C++ build) and permanent (the firewall
rule is tied to the exe's path). Some antivirus products (Avast, for one) quarantine freshly built, unsigned
programs that open network ports; if an exe disappears after a build, restore it and add an exception.

## Test from the command line

```
crtcast_test 192.168.0.13 --probe                 # INIT + CLOSE only, no mode change
crtcast_test 192.168.0.13                         # 640x480i, RGB888, audio, until Enter
crtcast_test 192.168.0.13 --mode 0 --565          # 320x240p, RGB565
crtcast_test --airplay 60                         # AirPlay receiver only, prints status
```

Flags: `--mode 0|1|2` (320x240p, 640x240p, 640x480i), `--565`, `--lz4`, `--jumbo`, `--swap`, `--noaudio`,
`--delay ms`, `--seconds n`, `--cycles n` (start/stop n times in one process).

## Known issues

- **LZ4 compression is off on Windows.** With LZ4, 480i jitters up and down on the CRT, with RGB888 and
  RGB565, with Registered I/O or plain sockets, and with raster sync line 1 or automatic. The Mac app doesn't
  show it with the same core. Field timing and parity recorded on the PC look identical with and without
  LZ4; the cause hasn't been found. Jumbo frames and field swapping are also left off.
- PIN pairing isn't supported; plain screen mirroring works without it.
- Video files aren't supported yet (AirPlay mirroring only).

## Diagnostics

- `build.ps1 -NoRio` builds groovymister with plain sockets instead of Registered I/O (a patched copy is
  generated at build time; the source stays unchanged).
- `CRTCAST_FIELD_LOG=<file.csv>`: records every field's timing, the MiSTer's counters and the field parity.
- `CRTCAST_VSYNC_LINE=<n>`: overrides the raster sync line in each blit header (default 1, 0 = automatic).
- **Show AirPlay log** in the settings panel shows the receiver's recent log.

## License

| Part | Folder | License |
|---|---|---|
| UxPlay AirPlay receiver core | `shared/AirPlay/UxPlay/`, `core/airplay/uxplay_win/` | GPL-3.0 |
| Groovy_MiSTer sender library | `shared/Groovy/` | GPL-2.0 |
| LZ4 | `shared/Groovy/lz4/` | BSD-2-Clause |
| llhttp | `shared/AirPlay/UxPlay/llhttp/` | MIT |
| FFmpeg (linked, decoders only) | via vcpkg | LGPL-2.1+ |
| vcpkg FFmpeg recipe | `vcpkg-ports/ffmpeg/` | MIT |
| CRTCast's own code | everything else | GPL-3.0 |

Note: Groovy_MiSTer's sources don't say "GPL-2.0 or later". GPL-2.0-only code can't be combined with GPL-3.0
code in one distributed program, so distributing builds that include both needs the Groovy author's OK for
"or later" (or the AirPlay part removed). The top-level `LICENSE` is GPL-3.0.
