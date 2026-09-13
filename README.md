# UPH - Unreal Project Handler

Native C++ desktop application using Dear ImGui and SDL3.

## macOS

Prerequisites: Xcode Command Line Tools, Homebrew, and SDL3.

```sh
brew install sdl3
make
make run
```

The binary is written to `build/uph`.

## Windows (Scoop + vcpkg)

Install the build tools and SDL3:

```powershell
scoop install make gcc pkgconf vcpkg
vcpkg install sdl3:x64-mingw-dynamic
```

Build from PowerShell:

```powershell
$gccBin = "$env:USERPROFILE\scoop\apps\gcc\current\bin"
$vcpkgRoot = "$env:USERPROFILE\scoop\apps\vcpkg\current\installed\x64-mingw-dynamic"
$env:PATH = "$gccBin;$vcpkgRoot\bin;$env:PATH"
$env:PKG_CONFIG_PATH = "$vcpkgRoot\lib\pkgconfig"
make -f Makefile.windows
make -f Makefile.windows run
```

The executable is written to `build/uph.exe`.
