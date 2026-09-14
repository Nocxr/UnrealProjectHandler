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
