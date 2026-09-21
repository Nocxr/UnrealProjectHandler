CXX ?= clang++
CC ?= clang
IMGUI_DIR := third_party/imgui
SDL_DIR := third_party/SDL
SDL_BUILD_DIR := build/SDL

APP_TARGET := build/uph-app
CLI_TARGET := build/uph
TARGETS := $(APP_TARGET) $(CLI_TARGET)

APP_SOURCES := src/main.cpp \
	$(IMGUI_DIR)/imgui.cpp \
	$(IMGUI_DIR)/imgui_draw.cpp \
	$(IMGUI_DIR)/imgui_tables.cpp \
	$(IMGUI_DIR)/imgui_widgets.cpp \
	$(IMGUI_DIR)/backends/imgui_impl_sdl3.cpp \
	$(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

CLI_SOURCES := src/cli_main.cpp src/unreal_file_index.cpp

WINDOWS_RESOURCES :=
SDL_TARGETS :=
RUNTIME_FILES :=
SDL_CMAKE_GENERATOR :=
APP_SUBSYSTEM :=
CLI_SUBSYSTEM :=
CLI_LIBS :=

ifeq ($(OS),Windows_NT)
    SCOOP_ROOT ?= $(USERPROFILE)/scoop
    GCC_BIN ?= $(SCOOP_ROOT)/apps/gcc/current/bin
    MAKE_PROGRAM ?= $(SCOOP_ROOT)/shims/make.exe
    ifneq ($(wildcard $(GCC_BIN)/gcc.exe),)
        CC := $(GCC_BIN)/gcc.exe
    endif
    ifneq ($(wildcard $(GCC_BIN)/g++.exe),)
        CXX := $(GCC_BIN)/g++.exe
        export PATH := $(GCC_BIN);$(PATH)
    endif
    ifneq ($(wildcard $(GCC_BIN)/windres.exe),)
        WINDRES := $(GCC_BIN)/windres.exe
    endif
    WINDRES ?= windres

    WINDOWS_RESOURCES := build/uph.res
    SDL_TARGETS := $(SDL_BUILD_DIR)/libSDL3.dll.a $(SDL_BUILD_DIR)/SDL3.dll
    RUNTIME_FILES := build/SDL3.dll
    SDL_CMAKE_GENERATOR := MinGW Makefiles

    APP_TARGET := build/uph-app.exe
    CLI_TARGET := build/uph.exe
    TARGETS := $(APP_TARGET) $(CLI_TARGET)

    SDL_CFLAGS := -I$(SDL_DIR)/include
    APP_LIBS := -L$(SDL_BUILD_DIR) -lSDL3 -lopengl32 -lshell32
    CLI_LIBS := -lshell32 -lole32 -luuid -luser32 -ladvapi32
    APP_SUBSYSTEM := -mwindows
    CLI_SUBSYSTEM := -mconsole

    MKDIR := if not exist build mkdir build
    CLEAN_UPH := if exist build\uph.exe del /Q build\uph.exe & if exist build\uph-app.exe del /Q build\uph-app.exe & if exist build\uph.res del /Q build\uph.res
    DEEP_CLEAN := if exist build rmdir /S /Q build
else
    SDL_TARGETS := $(SDL_BUILD_DIR)/libSDL3.dylib
    SDL_CMAKE_GENERATOR := Unix Makefiles
    MAKE_PROGRAM ?= make
    SDL_CFLAGS := -I$(SDL_DIR)/include
    APP_LIBS := -L$(SDL_BUILD_DIR) -Wl,-rpath,$(abspath $(SDL_BUILD_DIR)) -lSDL3 \
		-framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
    CLI_LIBS :=
    MKDIR := mkdir -p build
    CLEAN_UPH := $(RM) -f $(APP_TARGET) $(CLI_TARGET)
    DEEP_CLEAN := $(RM) -r build
endif

COMMON_CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wpedantic
APP_CXXFLAGS := $(COMMON_CXXFLAGS) $(SDL_CFLAGS) -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends
CLI_CXXFLAGS := $(COMMON_CXXFLAGS)

.PHONY: all app cli run install clean clean-all rebuild
all: app cli

app: $(APP_TARGET)

cli: $(CLI_TARGET)

$(APP_TARGET): $(APP_SOURCES) $(WINDOWS_RESOURCES) $(SDL_TARGETS) $(RUNTIME_FILES)
	@$(MKDIR)
	$(CXX) $(APP_CXXFLAGS) $(APP_SOURCES) $(WINDOWS_RESOURCES) -o $@ $(APP_LIBS) $(APP_SUBSYSTEM)

$(CLI_TARGET): $(CLI_SOURCES)
	@$(MKDIR)
	$(CXX) $(CLI_CXXFLAGS) $(CLI_SOURCES) -o $@ $(CLI_LIBS) $(CLI_SUBSYSTEM)

$(SDL_TARGETS): $(SDL_DIR)/CMakeLists.txt
	cmake -S $(SDL_DIR) -B $(SDL_BUILD_DIR) -G "$(SDL_CMAKE_GENERATOR)" -DCMAKE_MAKE_PROGRAM="$(MAKE_PROGRAM)" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$(CC)" -DCMAKE_CXX_COMPILER="$(CXX)" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF
	cmake --build $(SDL_BUILD_DIR) --config Release

ifeq ($(OS),Windows_NT)
build/SDL3.dll: $(SDL_BUILD_DIR)/SDL3.dll
	@$(MKDIR)
	powershell -NoProfile -Command "Copy-Item -Force '$<' '$@'"

$(WINDOWS_RESOURCES): src/uph.rc src/uph.ico
	@$(MKDIR)
	$(WINDRES) $< -O coff -o $@

install: all
	powershell -NoProfile -ExecutionPolicy Bypass -Command "$$dest=Join-Path $$env:LOCALAPPDATA 'UPH\bin'; New-Item -ItemType Directory -Force -Path $$dest | Out-Null; Copy-Item -Force '$(CLI_TARGET)' (Join-Path $$dest 'uph.exe'); Copy-Item -Force '$(APP_TARGET)' (Join-Path $$dest 'uph-app.exe'); Copy-Item -Force 'build/SDL3.dll' (Join-Path $$dest 'SDL3.dll'); $$userPath=[Environment]::GetEnvironmentVariable('Path','User'); $$parts=@($$userPath -split ';' | Where-Object { $$_ }); if ($$parts -notcontains $$dest) { [Environment]::SetEnvironmentVariable('Path', (($$parts + $$dest) -join ';'), 'User'); Write-Host 'Added' $$dest 'to your user PATH. Open a new terminal before using uph globally.' } else { Write-Host 'UPH bin is already on your user PATH.' }; Write-Host 'Installed UPH to' $$dest"
else
install: all
	@mkdir -p "$$HOME/.local/bin"
	@cp -f "$(CLI_TARGET)" "$$HOME/.local/bin/uph"
	@cp -f "$(APP_TARGET)" "$$HOME/.local/bin/uph-app"
	@echo "Installed UPH to $$HOME/.local/bin (ensure it is on PATH)."
endif

run: all
	./$(CLI_TARGET)

clean:
	$(CLEAN_UPH)

clean-all:
	$(DEEP_CLEAN)

rebuild: clean
	$(MAKE) all
