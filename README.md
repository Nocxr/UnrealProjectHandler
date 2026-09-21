# UPH - Unreal Project Handler

Native C++ desktop application using Dear ImGui and SDL3.

## macOS

Prerequisites: Xcode Command Line Tools and CMake.

```sh
git submodule update --init --recursive
make
make run
```

The binary is written to `build/uph`.

## Windows

Install the build tools:

```powershell
scoop install make gcc
winget install Kitware.CMake
```

Build from PowerShell:

```powershell
git submodule update --init --recursive
make
make run
```

The executable is written to `build/uph.exe`.


## Terminal CLI

Windows builds produce a lightweight console client and the desktop/tray application:

```text
build/uph.exe      terminal CLI
build/uph-app.exe  desktop/tray application
```

Install both to `%LOCALAPPDATA%\UPH\bin` and add that directory to the user PATH:

```powershell
make install
```

Examples:

```powershell
uph
uph status
uph project list
uph project select Ulu
uph engine list
uph engine select 5.8
uph editor
uph open
uph run
uph build
uph build Shipping
uph package Android Development
```

When the desktop app is running, selection and action commands are routed through it so the GUI, logs, and process state stay in sync.

## Build targets

```powershell
make cli        # CLI only; no SDL/ImGui/OpenGL build
make app        # desktop app and GUI dependencies
make            # both
make clean      # UPH outputs only; keeps SDL build cache
make rebuild    # normal clean + rebuild
make clean-all  # full build wipe, including SDL
```
