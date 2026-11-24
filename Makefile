# MechVR.  make = debug build, make release = small build, make packed = self-extracting small build (Linux only).
# On Windows, run the same commands in an MSYS2 UCRT64 shell. From Linux, make windows cross-compiles a Windows build.
CC      := gcc
SRC     := $(wildcard src/*.c)
HDR     := $(wildcard src/*.h)
GLSL    := $(sort $(wildcard src/shaders/*.glsl))
STAGES  := $(filter-out src/shaders/common.glsl,$(GLSL))
EMBED   := $(GLSL) assets/shapes.txt
WARN    := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
ifeq ($(OS),Windows_NT)
# Windows (MSYS2): the OpenXR loader is opened when the game starts in the headset, so it is not linked here.
EXE     := .exe
LIBS    := -lopengl32 -lgdi32 -lwinmm
SMALL   := -Os -ffunction-sections -fdata-sections -ffast-math
SMALLLD := -Wl,--gc-sections -s -static
else
EXE     :=
LIBS    := -lopenxr_loader -lGL -lX11 -lasound -lpthread -lm
SMALL   := -Os -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-stack-protector -fno-plt \
           -ffunction-sections -fdata-sections -fmerge-all-constants -fno-ident -ffast-math
SMALLLD := -Wl,--gc-sections,--build-id=none,-z,norelro,--hash-style=gnu,-O1 -s
endif

all: mechvr$(EXE)

build:
	@mkdir -p build/chk

# The shaders and the shapes file are embedded into the program.
# Each shader is checked by the reference GLSL compiler, and the shapes file by its own tool, before they are embedded.
build/shaders.ok: $(GLSL) | build
	@for f in $(STAGES); do \
	  n=$$(basename $$f .glsl); \
	  (echo '#version 450 core'; cat src/shaders/common.glsl $$f) > build/chk/$$n; \
	  glslangValidator build/chk/$$n > build/chk/$$n.log 2>&1 || { cat build/chk/$$n.log; exit 1; }; \
	  (echo '#version 450 core'; echo '#define EXTRA'; cat src/shaders/common.glsl $$f) > build/chk/$$n; \
	  glslangValidator build/chk/$$n > build/chk/$$n.log 2>&1 || { cat build/chk/$$n.log; exit 1; }; \
	  (echo '#version 450 core'; echo '#define SCREEN'; cat src/shaders/common.glsl $$f) > build/chk/$$n; \
	  glslangValidator build/chk/$$n > build/chk/$$n.log 2>&1 || { cat build/chk/$$n.log; exit 1; }; \
	done; touch $@

build/shapes.ok: assets/shapes.txt tools/mechvr_blender.py | build
	@python3 tools/mechvr_blender.py check assets/shapes.txt > build/shapes.log || { cat build/shapes.log; exit 1; }
	@touch $@

build/shaders.c: $(EMBED) tools/embed.py build/shaders.ok build/shapes.ok
	python3 tools/embed.py $@ $(EMBED)

build/shaders_min.c: $(EMBED) tools/embed.py build/shaders.ok build/shapes.ok
	python3 tools/embed.py --min $@ $(EMBED)

mechvr$(EXE): $(SRC) $(HDR) build/shaders.c
	$(CC) $(WARN) -O2 -g -Isrc -o $@ $(SRC) build/shaders.c $(LIBS)

release: $(SRC) $(HDR) build/shaders_min.c
	$(CC) $(WARN) $(SMALL) -Isrc -o mechvr-release$(EXE) $(SRC) build/shaders_min.c $(LIBS) $(SMALLLD)
ifneq ($(OS),Windows_NT)
	@strip -R .comment -R .gnu.version -R .note.gnu.property -R .eh_frame_hdr mechvr-release 2>/dev/null || true
endif
	@echo "release build: $$(stat -c %s mechvr-release$(EXE)) bytes"

ifeq ($(OS),Windows_NT)
packed:
	@echo "make packed makes a Linux shell script; on Windows use make release"; exit 1
else
# Self-extracting build: a tiny shell header followed by the xz-compressed program.
packed: release
	@printf '#!/bin/sh\nt=$$(mktemp);tail -n+3 "$$0"|xz -d>$$t;chmod +x $$t;$$t "$$@";r=$$?;rm -f $$t;exit $$r\n' > mechvr-packed
	@xz -9e --lzma2=preset=9e,lc=1,pb=0 -c mechvr-release >> mechvr-packed
	@chmod +x mechvr-packed
	@echo "packed build:  $$(stat -c %s mechvr-packed) bytes"
endif

# ---- A Windows build made on Linux: make windows ----
# Needs a MinGW-w64 cross compiler (Arch: mingw-w64-gcc, Debian/Ubuntu: gcc-mingw-w64-x86-64, or llvm-mingw);
# set WINCC to use another one. The result is a folder and a zip ready to hand out. The game needs
# openxr_loader.dll beside it for the headset (not for --flat): point OPENXR_DLL at x64/bin/openxr_loader.dll
# from the Khronos OpenXR SDK release (openxr_loader_windows-*.zip) to include it.
WINCC      ?= x86_64-w64-mingw32-gcc
XR_HEADERS ?= /usr/include/openxr
OPENXR_DLL ?=
OPENXR_LICENSE ?= /usr/share/doc/openxr/LICENSE
WINDIST    := dist/MechVR-windows

build/win-include/openxr/openxr.h: | build
	@mkdir -p build/win-include
	cp -r $(XR_HEADERS) build/win-include/

windows: $(SRC) $(HDR) build/shaders_min.c build/win-include/openxr/openxr.h
	@rm -rf $(WINDIST) dist/MechVR-windows.zip
	@mkdir -p $(WINDIST)
	$(WINCC) $(WARN) -O2 -Isrc -Ibuild/win-include -o $(WINDIST)/mechvr.exe $(SRC) build/shaders_min.c \
	    -lopengl32 -lgdi32 -lwinmm -s -static
	cp tools/windows/play-vr.bat tools/windows/play-in-window.bat tools/windows/README.txt $(WINDIST)/
	cp LICENSE $(WINDIST)/LICENSE.txt
ifneq ($(OPENXR_DLL),)
	cp $(OPENXR_DLL) $(WINDIST)/openxr_loader.dll
	cp $(OPENXR_LICENSE) $(WINDIST)/openxr_loader-LICENSE.txt # Apache 2.0: its license travels with it
else
	@echo "note: no OPENXR_DLL given, so the zip plays in a window only until openxr_loader.dll is added"
endif
	cd dist && python3 -m zipfile -c MechVR-windows.zip MechVR-windows
	@echo "windows build: $(WINDIST)/mechvr.exe, $$(stat -c %s $(WINDIST)/mechvr.exe) bytes; zip: dist/MechVR-windows.zip"

clean:
	rm -rf build dist mechvr mechvr-release mechvr-packed mechvr.exe mechvr-release.exe

.PHONY: all release packed windows clean
