

<!-- BEGIN CXX MAKE STANDARD -->
## C and C++ project build standard

For existing and new C/C++ projects, use the shared GNU Make command format. CMake language identifiers are C and CXX; CXX means C++.

- Keep project settings at the top, then shared tools/settings, OS commands, phony commands, and project-specific extensions. Use a standalone root Makefile and checked-in scripts; do not depend on an absolute path to another repo.
- `.DEFAULT_GOAL := all`, `all: build`. Plain `make` configures and builds. Prefer CMake with Ninja, Release by default, quoted paths, parallel compilation, and CMAKE_EXPORT_COMPILE_COMMANDS enabled in CMake for new projects.
- Shared commands: help, configure, build, run, stop, kill, clean, rebuild, debug, release, test, install, uninstall. `run` builds then launches. `stop` and `kill` are aliases and identify the executable from this checkout. `clean` stops then removes only a verified build directory inside the project. `rebuild` cleans then builds without launching. Debug/release select CONFIG and build without launching.
- Variables: PROJECT_NAME, APP_NAME, COMMAND_NAME, CMAKE, CTEST, CONFIG (Release default), GENERATOR (Ninja default), BUILD_DIR (build default, existing Mac build-mac conventions allowed), ARGS, CMAKE_ARGS, BUILD_ARGS. Do not silently ignore configuration overrides. Paths containing spaces must work.
- Install means registering a user-level command that runs this checkout's built application, preserving its runtime assets in the build tree. Windows uses managed launchers in LOCALAPPDATA/CxxTools/bin and one user PATH entry; reinstall replaces a managed launcher after a checkout moves and cleans narrowly identifiable old project/build PATH entries. Unix uses ~/.local/bin links; tell the user when it needs to be added to PATH. Uninstall removes only managed commands and preserves other projects' commands. Never change system PATH or overwrite unmanaged launchers.
- Stop a running Windows executable before rebuilding to avoid file locks. Launch desktop apps without exposing a terminal window. Preserve command-line arguments and project launch conventions.
- Use `cmake --build` with CONFIG and parallelism, and set CMAKE_BUILD_TYPE for single-config generators. Preserve supported OSes, actual executable names, Mac app bundles, runtime dependencies, and project-specific commands such as Dashboard module builds. Module-only builds must leave the Dashboard host running.
- Preserve a working custom compiler backend when one exists. Add an adapter to the shared commands; do not force a CMake rewrite solely for uniformity. Make configuration changes actually rebuild the affected targets.
- Test runs the existing test runner; fail clearly when no tests exist instead of reporting a successful empty run. Do not invent tests solely for Makefile uniformity.
- Keep .PHONY complete and sequential dependencies safe under `make -j`. Do not have sibling prerequisites race stop/build or clean/build. Provide help for all shared commands and any platform limitations.
- For existing projects, preserve unrelated source, user changes, settings, and build scripts unless adapting those scripts is necessary. Check existing CMake cache generators before switching them; explain incompatible caches instead of silently deleting them.
- Validate Makefile parsing, default build selection, dry runs of relevant commands, script syntax, and the actual build when tools are available. Inspect install/uninstall logic without modifying PATH as part of tests. Report what could not be verified.

For new C/C++ projects, use this repository's root Makefile and scripts as the starting point. Set the project/executable names and supported platforms instead of copying stale project settings.
<!-- END CXX MAKE STANDARD -->
