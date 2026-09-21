# UPH - Unreal Project Handler

Native C++ desktop application using Dear ImGui and SDL3.

## Windows

Install the build tools:

```powershell
scoop install make gcc
winget install Kitware.CMake
```

Build and run:

```powershell
git submodule update --init --recursive
make
make run
```

Windows builds two executables:

- `build/uph.exe` - terminal/CLI entry point
- `build/uph-app.exe` - tray/UI application

Running `uph.exe` with no arguments starts the tray application. If UPH is already running, it restores and foregrounds the existing window instead of starting another copy.

Install UPH globally for the current Windows user:

```powershell
make install
```

This copies `uph.exe`, `uph-app.exe`, and `SDL3.dll` to `%LOCALAPPDATA%\UPH\bin` and adds that directory to the user `PATH`. Open a new terminal after the first install, then `uph` can be called from anywhere.

## Terminal commands

```text
uph
uph status

uph project list
uph project current
uph project select Ulu
uph project select H:\projects\unreal\Ulu\Ulu.uproject

uph engine list
uph engine current
uph engine select 5.8
uph engine select H:\unreal\UE_5.8

uph editor
uph editor list
uph editor current
uph editor select 5.8

uph open
uph open Ulu
uph run

uph build
uph build Shipping
uph build Ulu Development
uph build --project Ulu --config Development

uph package
uph package Android
uph package Android Shipping
uph package Ulu Android Development
uph package --project Ulu --platform Android --config Shipping
uph package --platform Win64 --config Development --output H:\Builds\Ulu
```

If the current directory contains exactly one `.uproject`, `open`, `run`, `build`, and `package` use that project for the command without changing the globally selected project.

Project and engine selections made with `uph project select` and `uph engine select` use the same saved UPH settings as the desktop application.

## macOS

Prerequisites: Xcode Command Line Tools and CMake.

```sh
git submodule update --init --recursive
make
make run
```

macOS also builds `build/uph` as the CLI launcher and `build/uph-app` as the application. `make install` copies both to `~/.local/bin`.
