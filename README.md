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

The Windows build provides a lightweight `uph.exe` terminal client alongside the SDL/ImGui desktop app `uph-app.exe`.

```powershell
uph                         # start or foreground UPH
uph status                  # saved settings + live app/operation/device state
uph logs                    # current UPH runtime log
uph logs --follow           # follow the running app log
uph stop                    # request stop for the tracked build/package operation
uph rerun                   # repeat the last CLI build/package/deploy
uph build [project] [config]
uph package [project] [platform] [config]
uph deploy [project] [config]
```

`uph deploy` is a UPH orchestration command rather than an ADB alias. It packages the project for Android, refreshes the produced Unreal Android artifacts, installs them to the device selected in UPH, and requests launch.

Build targets remain separate:

```powershell
make cli        # CLI only; no SDL/ImGui/OpenGL
make app        # desktop app
make            # both
make clean      # UPH outputs only, keep SDL cache
make clean-all  # full dependency/build wipe
```


### Interactive selection

`uph` includes its own terminal fuzzy picker; no `fzf` install is required.

```powershell
uph project select
uph engine select
```

Type to fuzzy-filter, use Up/Down to move, Enter to select, Esc to cancel, and Backspace to edit the filter. Supplying a selector still works normally, while an ambiguous selector such as `uph engine select 5.8` opens the picker pre-filtered to matching entries.
