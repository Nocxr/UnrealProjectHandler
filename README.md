# UPH - Unreal Project Handler

Native C++ desktop application using Dear ImGui and SDL3. Python and Tk are not required.

## macOS

Prerequisites: Xcode Command Line Tools, Homebrew, and SDL3.

```sh
brew install sdl3
make
make run
```

The binary is written to `build/uph`.

## Windows (MSYS2 MinGW64)

Install the compiler and SDL3 packages from an MSYS2 MinGW64 shell:

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL3 make pkgconf
make -f Makefile.windows
make -f Makefile.windows run
```

The executable is written to `build/uph.exe`.
