# LetraTo - 32-bit Windows build (MinGW-w64 i686 + NASM)
#
#   MSYS2 (MINGW32 shell):  pacman -S mingw-w64-i686-gcc mingw-w64-i686-SDL2 nasm make
#                           make
#   Linux cross-compile:    make CROSS=i686-w64-mingw32- SDL2_DIR=/path/to/SDL2-2.x/i686-w64-mingw32
#   Backend unit tests:     make test        (Linux, needs gcc-multilib)
#
# Output: build/LetraTo.exe, build/stroke.dll (+ SDL2.dll copied when SDL2_DIR is set)

CROSS    ?=
CXX      := $(CROSS)g++
CC       := $(CROSS)gcc
NASM     ?= nasm
BUILD    := build

ifdef SDL2_DIR
  SDL_CFLAGS := -I$(SDL2_DIR)/include/SDL2
  SDL_LIBS   := -L$(SDL2_DIR)/lib -lmingw32 -lSDL2main -lSDL2
else
  SDL_CFLAGS = $(shell sdl2-config --cflags)
  SDL_LIBS   = $(shell sdl2-config --libs)
endif

CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -m32 $(SDL_CFLAGS)
SRCS     := src/main.cpp src/glyphs.cpp src/pictures.cpp src/render.cpp src/audio.cpp
OBJS     := $(SRCS:src/%.cpp=$(BUILD)/%.o)

all: $(BUILD)/LetraTo.exe

# ---- assembly backend: pure-NASM DLL, no C runtime --------------------------
$(BUILD)/stroke.obj: asm/stroke.asm | $(BUILD)
	$(NASM) -f win32 $< -o $@

$(BUILD)/stroke.dll: $(BUILD)/stroke.obj asm/stroke.def
	$(CC) -m32 -shared -nostdlib -Wl,-e,_DllMain@12 \
	    -Wl,--out-implib,$(BUILD)/libstroke.dll.a $^ -o $@

# ---- C++ frontend ------------------------------------------------------------
$(BUILD)/%.o: src/%.cpp src/*.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/LetraTo.exe: $(OBJS) $(BUILD)/stroke.dll
	$(CXX) -m32 $(OBJS) -L$(BUILD) -lstroke $(SDL_LIBS) -mwindows -static-libgcc -static-libstdc++ -o $@
ifdef SDL2_DIR
	cp $(SDL2_DIR)/bin/SDL2.dll $(BUILD)/
else
	@cp /mingw32/bin/SDL2.dll $(BUILD)/ 2>/dev/null || true
	@cp /mingw32/bin/libwinpthread-1.dll $(BUILD)/ 2>/dev/null || true
endif

# ---- tests -------------------------------------------------------------------
test: | $(BUILD)
	$(NASM) -f elf32 asm/stroke.asm -o $(BUILD)/stroke_elf.o
	gcc -m32 -no-pie -std=c99 -D_DEFAULT_SOURCE tests/test_stroke.c $(BUILD)/stroke_elf.o -lm -o $(BUILD)/test_stroke
	./$(BUILD)/test_stroke

test-win: $(BUILD)/stroke.dll
	$(CC) -m32 -std=c99 tests/test_stroke.c -L$(BUILD) -lstroke -o $(BUILD)/test_stroke.exe

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all test test-win clean
