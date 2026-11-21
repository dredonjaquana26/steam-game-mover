# Building from source

**Requirements:** Windows + a C++ toolchain. No external libraries — only the Windows SDK headers that ship with the toolchain.

The app is a single native executable. It was built with **MinGW-w64** (GCC), which produces a tiny, fully static binary with no runtime to install.

### Build the icon resource (optional, once)

```bat
windres app.rc -O coff -o app_res.o
```

### Build the binary

```bat
g++ -O2 -std=c++17 -municode -static -static-libgcc -static-libstdc++ -mwindows ^
    -o SteamGameMover main.cpp app_res.o ^
    -lcomctl32 -lshell32 -lole32 -luuid -lgdiplus -ldwmapi -luxtheme -lgdi32 -luser32 -ladvapi32
```

Result: a single ~1 MB file that runs on any Windows 10/11 x64 PC with nothing to install. Visual-style theming and DPI awareness come from MinGW's default application manifest; if you build with MSVC instead, embed `app.manifest` (Common Controls v6 + PerMonitorV2).

### Developer flags

| Flag | What it does |
|------|--------------|
| `--selftest` | headless check: locate Steam, parse libraries + app manifests, list games → `%TEMP%\sgm_selftest.log` |
| `--render out.png [moving]` | render the interface to a PNG (`moving` = a copy-in-progress state) |

### How the move works

See the `MoveGame` function in `engine.h`: copy → verify (file count + bytes) → delete original → create an NTFS directory junction (via `DeviceIoControl` / `FSCTL_SET_REPARSE_POINT`, no admin) → roll back on any failure.
