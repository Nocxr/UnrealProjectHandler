# Project settings; keep shared command semantics identical across repos.
PROJECT_NAME := UnrealProjectHandler
APP_NAME := uph-app
COMMAND_NAME := uph-app
BACKEND := legacy
EXTRA_HELP := app cli service clean-all
EXTRA_EXECUTABLES = $(BUILD_DIR)/uph.exe;$(BUILD_DIR)/uph-index-service.exe

# Shared C/C++ commands. Project-specific settings live above this block.
.DEFAULT_GOAL := all
.NOTPARALLEL:
CMAKE ?= cmake
CTEST ?= ctest
CONFIG ?= Release
GENERATOR ?= Ninja
BUILD_DIR ?= build
ARGS ?=
CMAKE_ARGS ?=
BUILD_ARGS ?=
EXTRA_EXECUTABLES ?=
TEST_COMMAND ?= $(CTEST) --test-dir "$(BUILD_DIR)" -C "$(CONFIG)" --output-on-failure --no-tests=error

ifeq ($(OS),Windows_NT)
EXE ?= $(BUILD_DIR)/$(APP_NAME).exe
HELPER := powershell -NoProfile -ExecutionPolicy Bypass -File "scripts/Project-Commands.ps1"
STOP_CMD = $(HELPER) -Action Stop -Executable "$(EXE)" -ExtraExecutables "$(EXTRA_EXECUTABLES)"
RUN_CMD = $(HELPER) -Action Run -Executable "$(EXE)" -RunArguments '$(ARGS)'
CLEAN_CMD = $(HELPER) -Action Clean -BuildDir "$(BUILD_DIR)"
INSTALL_CMD ?= $(HELPER) -Action Install -Executable "$(EXE)" -CommandName "$(COMMAND_NAME)" -ProjectName "$(PROJECT_NAME)"
UNINSTALL_CMD ?= $(HELPER) -Action Uninstall -CommandName "$(COMMAND_NAME)"
else
UNAME_S := $(shell uname -s)
EXE ?= $(BUILD_DIR)/$(APP_NAME)
STOP_CMD = sh scripts/project-commands.sh stop "$(EXE)"
RUN_CMD = "$(EXE)" $(ARGS)
ifeq ($(UNAME_S),Darwin)
ifneq ($(MAC_APP),)
EXE := $(BUILD_DIR)/$(MAC_APP).app/Contents/MacOS/$(APP_NAME)
RUN_CMD = open "$(BUILD_DIR)/$(MAC_APP).app" --args $(ARGS)
endif
endif
CLEAN_CMD = sh scripts/project-commands.sh clean "$(BUILD_DIR)"
INSTALL_CMD ?= sh scripts/project-commands.sh install "$(EXE)" "$(COMMAND_NAME)"
UNINSTALL_CMD ?= sh scripts/project-commands.sh uninstall "$(EXE)" "$(COMMAND_NAME)"
endif

.PHONY: all help configure build run stop kill clean rebuild debug release test install uninstall
all: build

help:
	@echo "$(PROJECT_NAME): shared C/C++ Makefile commands"
	@echo "  make / make build   Configure and build (default: Release)"
	@echo "  make run            Build and launch; ARGS supplies arguments"
	@echo "  make stop / kill    Stop this checkout's running executable"
	@echo "  make clean          Stop and remove the build directory"
	@echo "  make rebuild        Clean and build; does not launch"
	@echo "  make debug/release  Build the selected configuration"
	@echo "  make test           Build and run existing tests; fail if none exist"
	@echo "  make install        Build and register a command pointing to this checkout"
	@echo "  make uninstall      Remove the registered command"
	@echo "  Overrides: CONFIG BUILD_DIR GENERATOR ARGS CMAKE_ARGS BUILD_ARGS"
	@echo "  Project targets: $(EXTRA_HELP)"

ifeq ($(BACKEND),legacy)
configure:
	@echo "$(PROJECT_NAME) uses its existing compiler backend; no separate configure step."
	$(CMAKE) -DCONFIG="$(CONFIG)" -DBUILD_DIR="$(BUILD_DIR)" -P scripts/Configure-UPH.cmake

build: stop configure
	$(MAKE) -f Makefile.backend all $(BUILD_ARGS)

test: build
	@echo "No automated test runner is configured for $(PROJECT_NAME)."
	@$(CMAKE) -E false

install: build
	$(INSTALL_CMD)
ifeq ($(OS),Windows_NT)
	$(HELPER) -Action Install -Executable "$(BUILD_DIR)/uph.exe" -CommandName "uph" -ProjectName "$(PROJECT_NAME)"
	$(HELPER) -Action Install -Executable "$(BUILD_DIR)/uph-index-service.exe" -CommandName "uph-index-service" -ProjectName "$(PROJECT_NAME)"
else
	sh scripts/project-commands.sh install "$(BUILD_DIR)/uph" uph
endif

uninstall:
	$(UNINSTALL_CMD)
ifeq ($(OS),Windows_NT)
	$(HELPER) -Action Uninstall -CommandName uph
	$(HELPER) -Action Uninstall -CommandName uph-index-service
else
	sh scripts/project-commands.sh uninstall "$(BUILD_DIR)/uph" uph
endif
else
configure:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE="$(CONFIG)" $(CMAKE_ARGS)

build: stop configure
	$(CMAKE) --build "$(BUILD_DIR)" --config "$(CONFIG)" --parallel $(BUILD_ARGS)

test: build
	$(TEST_COMMAND)

install: build
	$(INSTALL_CMD)

uninstall:
	$(UNINSTALL_CMD)
endif

run: build
	$(RUN_CMD)

stop:
	$(STOP_CMD)

kill: stop

clean: stop
	$(CLEAN_CMD)

rebuild: clean
	$(MAKE) build

debug:
	$(MAKE) build CONFIG=Debug

release:
	$(MAKE) build CONFIG=Release

# Preserve the existing UPH compiler backend and its multi-binary install.
.PHONY: app cli service clean-all
app cli service: stop
	$(MAKE) -f Makefile.backend $@
clean-all: clean
