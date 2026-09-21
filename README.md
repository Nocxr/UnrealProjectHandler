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


### Project and engine registration

Project and engine selection no longer starts the UPH desktop app just to change saved state. If UPH is already running, the CLI syncs through IPC; otherwise it updates the shared settings headlessly.

```powershell
uph project add
uph project add H:\projects\unreal\Ulu\Ulu.uproject
uph project remove
uph project remove Ulu

uph engine add
uph engine add H:\unreal\UE_5.8
uph engine remove
uph engine remove UE_5.8
```

With no argument, `project add` first checks the current directory for a single `.uproject`; if none is found it opens a Windows `.uproject` file picker rooted at the current directory. `engine add` first checks whether the current directory is an Unreal Engine root; otherwise it opens a folder picker. Remove with no argument uses the built-in fuzzy picker.

Removed discovered engines are persisted as hidden paths so launcher/registry discovery does not immediately add them back. Adding or selecting that engine again unhides it.

### Unreal file index

UPH can maintain a tiny machine-local index containing only Unreal project and plugin descriptors:

```powershell
uph index rebuild                 # index all fixed local drives
uph index rebuild H:\            # index one drive/root
uph index rebuild H:\projects D:\work
uph index status
uph index test                    # synthetic scanner/cache/search self-test
uph index clear

uph find Ulu
uph find ScriptRuntime --plugins
uph find Hollow --projects
uph find --limit 200              # interactive fuzzy picker over indexed files
```

On Windows, whole NTFS drive roots first use direct MFT enumeration via `FSCTL_ENUM_USN_DATA`, avoiding a normal recursive directory walk when raw-volume access is available. If Windows denies raw-volume access or a supplied root is not a whole NTFS drive, UPH falls back to a permission-tolerant directory scan. The resulting cache lives beside UPH's other per-user configuration files as `unreal-files.idx`.

Indexed projects are also included automatically in `uph project select`, so a project does not have to be manually registered before it can be found.
