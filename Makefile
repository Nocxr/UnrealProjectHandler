CXX ?= clang++
SDL_PREFIX ?= $(shell brew --prefix sdl3)
IMGUI_DIR := third_party/imgui
TARGET := build/uph

SOURCES := src/main.cpp \
	$(IMGUI_DIR)/imgui.cpp \
	$(IMGUI_DIR)/imgui_draw.cpp \
	$(IMGUI_DIR)/imgui_tables.cpp \
	$(IMGUI_DIR)/imgui_widgets.cpp \
	$(IMGUI_DIR)/backends/imgui_impl_sdl3.cpp \
	$(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wpedantic \
	-I$(SDL_PREFIX)/include -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends
LDFLAGS := -L$(SDL_PREFIX)/lib -Wl,-rpath,$(SDL_PREFIX)/lib -lSDL3 \
	-framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo

.PHONY: all run clean
all: $(TARGET)

$(TARGET): $(SOURCES)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $@ $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	$(RM) -r build
