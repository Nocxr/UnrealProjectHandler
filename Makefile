CXX ?= clang++
IMGUI_DIR := third_party/imgui
TARGET := build/uph

SOURCES := src/main.cpp \
	$(IMGUI_DIR)/imgui.cpp \
	$(IMGUI_DIR)/imgui_draw.cpp \
	$(IMGUI_DIR)/imgui_tables.cpp \
	$(IMGUI_DIR)/imgui_widgets.cpp \
	$(IMGUI_DIR)/backends/imgui_impl_sdl3.cpp \
	$(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

ifeq ($(OS),Windows_NT)
    CXX ?= g++
    TARGET := build/uph.exe
    SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
    SDL_LIBS := $(shell pkg-config --libs sdl3) -lopengl32 -lshell32 -mwindows
    MKDIR := if not exist build mkdir build
else
    SDL_PREFIX ?= $(shell brew --prefix sdl3)
    SDL_CFLAGS := -I$(SDL_PREFIX)/include
    SDL_LIBS := -L$(SDL_PREFIX)/lib -Wl,-rpath,$(SDL_PREFIX)/lib -lSDL3 \
		-framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
    MKDIR := mkdir -p build
endif

CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wpedantic $(SDL_CFLAGS) \
	-I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends
LDFLAGS := $(SDL_LIBS)

.PHONY: all run clean
all: $(TARGET)

$(TARGET): $(SOURCES)
	@$(MKDIR)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $@ $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	$(RM) -r build