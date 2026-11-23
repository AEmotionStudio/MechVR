# MechVR style test.  make = debug build, make release = small build, make packed = self-extracting small build.
CC      := gcc
SRC     := $(wildcard src/*.c)
HDR     := $(wildcard src/*.h)
GLSL    := $(sort $(wildcard src/shaders/*.glsl))
STAGES  := $(filter-out src/shaders/common.glsl,$(GLSL))
EMBED   := $(GLSL) assets/shapes.txt
LIBS    := -lopenxr_loader -lGL -lX11 -lasound -lpthread -lm
WARN    := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
SMALL   := -Os -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-stack-protector -fno-plt \
           -ffunction-sections -fdata-sections -fmerge-all-constants -fno-ident -ffast-math
SMALLLD := -Wl,--gc-sections,--build-id=none,-z,norelro,--hash-style=gnu,-O1 -s

all: mechvr

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
	done; touch $@

build/shapes.ok: assets/shapes.txt tools/mechvr_blender.py | build
	@python3 tools/mechvr_blender.py check assets/shapes.txt > build/shapes.log || { cat build/shapes.log; exit 1; }
	@touch $@

build/shaders.c: $(EMBED) tools/embed.py build/shaders.ok build/shapes.ok
	python3 tools/embed.py $@ $(EMBED)

build/shaders_min.c: $(EMBED) tools/embed.py build/shaders.ok build/shapes.ok
	python3 tools/embed.py --min $@ $(EMBED)

mechvr: $(SRC) $(HDR) build/shaders.c
	$(CC) $(WARN) -O2 -g -Isrc -o $@ $(SRC) build/shaders.c $(LIBS)

release: $(SRC) $(HDR) build/shaders_min.c
	$(CC) $(WARN) $(SMALL) -Isrc -o mechvr-release $(SRC) build/shaders_min.c $(LIBS) $(SMALLLD)
	@strip -R .comment -R .gnu.version -R .note.gnu.property -R .eh_frame_hdr mechvr-release 2>/dev/null || true
	@echo "release build: $$(stat -c %s mechvr-release) bytes"

# Self-extracting build: a tiny shell header followed by the xz-compressed program.
packed: release
	@printf '#!/bin/sh\nt=$$(mktemp);tail -n+3 "$$0"|xz -d>$$t;chmod +x $$t;$$t "$$@";r=$$?;rm -f $$t;exit $$r\n' > mechvr-packed
	@xz -9e --lzma2=preset=9e,lc=1,pb=0 -c mechvr-release >> mechvr-packed
	@chmod +x mechvr-packed
	@echo "packed build:  $$(stat -c %s mechvr-packed) bytes"

clean:
	rm -rf build mechvr mechvr-release mechvr-packed

.PHONY: all release packed clean
