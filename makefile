#
# Makefile for OpenRAR (POSIX / Linux / macOS)
#

CXX ?= c++
CXXFLAGS ?= -O3
CXXFLAGS += -std=c++17 -Wno-logical-op-parentheses -Wno-switch -Wno-dangling-else
DEFINES = -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -DRAR_SMP
INCLUDES = -Isrc -Isrc/archive -Isrc/cli -Isrc/compress -Isrc/core -Isrc/crypto -Isrc/format -Isrc/io -Isrc/recovery
STRIP ?= strip
LDFLAGS += -pthread
DESTDIR ?= /usr

COMPILE = $(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEFINES) $(INCLUDES)

SOURCES = \
	src/core/cpu.cpp \
	src/core/vint.cpp \
	src/crypto/aes256.cpp \
	src/crypto/blake2sp.cpp \
	src/crypto/crc32.cpp \
	src/crypto/pbkdf2.cpp \
	src/crypto/sha256.cpp \
	src/io/file_stream.cpp \
	src/io/path_util.cpp \
	src/io/win32_meta.cpp \
	src/format/header_reader.cpp \
	src/format/header_writer.cpp \
	src/compress/filters50.cpp \
	src/compress/compressor50.cpp \
	src/compress/decompressor50.cpp \
	src/archive/archive_reader.cpp \
	src/archive/archive_mutator.cpp \
	src/recovery/rs16.cpp \
	src/recovery/recovery_record.cpp \
	src/cli/main.cpp

CORE_SOURCES = $(filter-out src/cli/main.cpp,$(SOURCES))
OBJECTS = $(SOURCES:.cpp=.o)
CORE_OBJECTS = $(CORE_SOURCES:.cpp=.o)

all: openrar

%.o: %.cpp
	$(COMPILE) -c $< -o $@

openrar: $(OBJECTS)
	$(CXX) -o $@ $(OBJECTS) $(LDFLAGS) $(LIBS)

clean:
	rm -f $(OBJECTS) openrar

install: openrar
	install -D openrar $(DESTDIR)/bin/openrar

uninstall:
	rm -f $(DESTDIR)/bin/openrar

# ── WASM (Emscripten) — delegates to the CMake presets ──────────────────────
# The presets in CMakePresets.json are the single source of truth for wasm
# compile/link flags. A second hand-rolled emcc flag set here is how the dist
# artifacts remain consistent with the wrappers.
#
# Requires emsdk (pinned: 3.1.50 — same version CI uses):
#   source emsdk/emsdk_env.sh   (provides emcmake / em++)
#
# Usage:
#   make wasm           → wasm/dist/openrar.{js,wasm}         (block codec)
#   make wasm-archive   → wasm/dist/openrar_archive.{js,wasm} (RAR5 archive API)
#   make wasm-cli       → wasm/dist/openrar_cli.{js,wasm}     (full CLI + MEMFS)
#   make wasm-debug     → unoptimized block codec
EMCMAKE ?= emcmake

wasm:
	$(EMCMAKE) cmake --preset wasm
	cmake --build --preset wasm

wasm-archive:
	$(EMCMAKE) cmake --preset wasm-archive
	cmake --build --preset wasm-archive

wasm-cli:
	$(EMCMAKE) cmake --preset wasm-cli
	cmake --build --preset wasm-cli

wasm-debug:
	$(EMCMAKE) cmake --preset wasm-debug
	cmake --build --preset wasm-debug

clean-wasm:
	rm -rf build/wasm build/wasm-debug build/wasm-cli build/wasm-archive

.PHONY: all clean install uninstall wasm wasm-archive wasm-cli wasm-debug clean-wasm
