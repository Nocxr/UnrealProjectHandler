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

On Windows, the recommended mode is the small privileged **UPH Unreal File Index** service. UPH itself stays non-admin. The service performs a fast MFT snapshot of fixed NTFS volumes, then keeps the index current from the NTFS USN change journal.

```powershell
make cli
.\build\uph.exe index service install   # one UAC prompt; installs + starts service
.\build\uph.exe index service status
.\build\uph.exe index status

.\build\uph.exe find Ulu
.\build\uph.exe find ScriptRuntime --plugins
.\build\uph.exe find Hollow --projects
.\build\uph.exe find --limit 200
```

Service lifecycle commands are:

```powershell
uph index service status
uph index service install
uph index service start
uph index service stop
uph index service uninstall
```

The service is registered for automatic Windows startup. Its installed executable, shared cache, status file, and log live under `%PROGRAMDATA%\UnrealProjectHandler\`. Search commands automatically prefer the shared service cache when it exists.

The service keeps a file-reference/directory map in memory, so `.uproject` / `.uplugin` creates, deletes, moves, file renames, and parent-directory renames can be applied incrementally without rescanning the drive. If the USN journal is replaced or falls behind, that volume is automatically rebuilt from the MFT.

The raw service cache deliberately keeps every descriptor it can see. Normal `uph find` and `uph project select` filter that raw cache into a useful user view: Unreal Engine installations/source trees, editor history, generated `HostProject` trees, cache/build directories, and invalid empty descriptor names are hidden. Use `uph find --engine` for engine-provided descriptors or `uph find --all` for the literal raw cache.

Manual indexing is still supported for tests and smaller roots:

```powershell
uph index rebuild H:\projects
uph index rebuild D:\work\SomeTree
uph index status
uph index test
uph index clear
```

A whole NTFS drive no longer falls back to a potentially multi-minute recursive directory walk when raw-volume access is denied. Use the index service for whole-drive indexing. Non-drive-root paths still use the permission-tolerant directory scanner.

Indexed projects are included automatically in `uph project select`, so a project does not have to be manually registered before it can be found.
