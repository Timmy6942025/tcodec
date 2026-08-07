# ────────────────────────────────────────────────────────────────
# TCodec Makefile
#
# Auto-detects ARM NEON and builds accordingly.
# On ARM (aarch64): NEON always available, -march=armv8-a
# On x86_64:        Scalar-only fallback (for development/testing)
#
# Targets:
#   make          — Build static library + CLI tools
#   make shared   — Build shared library
#   make test     — Build and run the bounded 50-test regression
#   make test-full — Build and run all 51 tests, including the long soak
#   make clean    — Remove build artifacts
#   make bench    — Run benchmarks
# ────────────────────────────────────────────────────────────────

# ── Configuration ───────────────────────────────────────────────

CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -O2
LDFLAGS ?=

# ── Architecture detection ──────────────────────────────────────

UNAME_M := $(shell uname -m)

ifeq ($(UNAME_M),aarch64)
    # ARM64 — NEON always available
    ARCH_CFLAGS  = -march=armv8-a -DTCODEC_NEON=1
    ARCH_LDFLAGS = -lpthread
    NEON_SRC     = $(wildcard neon/*.c)
else ifeq ($(UNAME_M),arm64)
    # Apple Silicon (macOS) — NEON always available
    ARCH_CFLAGS  = -DTCODEC_NEON=1
    ARCH_LDFLAGS = -lpthread
    NEON_SRC     = $(wildcard neon/*.c)
else ifeq ($(UNAME_M),armv7l)
    # ARM32 with NEON
    ARCH_CFLAGS  = -march=armv7-a -mfpu=neon -DTCODEC_NEON=1
    ARCH_LDFLAGS = -lpthread
    NEON_SRC     = $(wildcard neon/*.c)
else
    # x86_64 / other — scalar fallback
    ARCH_CFLAGS  = -DTCODEC_NEON=0
    ARCH_LDFLAGS = -lpthread
    NEON_SRC     =
endif

# ── Paths ───────────────────────────────────────────────────────

SRC_DIR  = src
NEON_DIR = neon
INC_DIR  = include
TOOL_DIR = tools
TEST_DIR = test
BUILD_DIR = build

# ── Source files ─────────────────────────────────────────────────

CORE_SRC = $(wildcard $(SRC_DIR)/*.c)
ALL_SRC  = $(CORE_SRC) $(NEON_SRC)

CORE_OBJ = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(CORE_SRC))
NEON_OBJ = $(patsubst $(NEON_DIR)/%.c,$(BUILD_DIR)/%.o,$(NEON_SRC))
ALL_OBJ  = $(CORE_OBJ) $(NEON_OBJ)

TOOL_SRC = $(TOOL_DIR)/tcenc.c $(TOOL_DIR)/tcdec.c $(TOOL_DIR)/tcmux.c
TOOL_OBJ = $(BUILD_DIR)/tcenc.o $(BUILD_DIR)/tcdec.o $(BUILD_DIR)/tcmux.o

TEST_SRC = $(TEST_DIR)/test_tcodec.c
TEST_OBJ = $(BUILD_DIR)/test_tcodec.o

# ── Targets ──────────────────────────────────────────────────────

LIB_STATIC = $(BUILD_DIR)/libtcodec.a
LIB_SHARED = $(BUILD_DIR)/libtcodec.so
ENC_BIN    = $(BUILD_DIR)/tcenc
DEC_BIN    = $(BUILD_DIR)/tcdec
MUX_BIN    = $(BUILD_DIR)/tcmux
TEST_BIN   = $(BUILD_DIR)/test_tcodec

# ── Flags ────────────────────────────────────────────────────────

WARN_FLAGS = -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare

COMMON_CFLAGS = $(CFLAGS) $(ARCH_CFLAGS) $(WARN_FLAGS) \
                -I$(INC_DIR) -std=c11 -D_GNU_SOURCE

RELEASE_CFLAGS = $(COMMON_CFLAGS) -DNDEBUG -flto
SAN_FLAGS     ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined \
                 -fno-omit-frame-pointer
DEBUG_CFLAGS   = $(COMMON_CFLAGS) -g -DTCODEC_DEBUG $(SAN_FLAGS)

# ── Default: release build ───────────────────────────────────────

.PHONY: all clean shared test test-fast test-full bench soak-1080p test-mp4 debug release nothreads cross-test cross-test-info install soaks-1080p test-common-deps test-full-deps

SOAK_SCRIPT = $(TOOL_DIR)/soak_1080p.sh
SOAK_FRAMES ?= $(or $(TCODEC_SOAK_FRAMES),300)
SOAK_QP ?= $(or $(TCODEC_SOAK_QP),42)
SOAK_THREADS ?= $(or $(TCODEC_SOAK_THREADS),1)
SOAK_OUT ?= $(or $(TCODEC_SOAK_OUT),/tmp/tcodec-soak)

soak-1080p: release
	TCODEC_SOAK_FRAMES=$(SOAK_FRAMES) TCODEC_SOAK_QP=$(SOAK_QP) \
	TCODEC_SOAK_THREADS=$(SOAK_THREADS) TCODEC_SOAK_OUT=$(SOAK_OUT) \
	sh $(SOAK_SCRIPT)

# Backward-compatible spelling used by some benchmark notes.
soaks-1080p: soak-1080p

all: release

release: $(LIB_STATIC) $(ENC_BIN) $(DEC_BIN) $(MUX_BIN)

debug: CFLAGS := -O0
debug: COMMON_CFLAGS := $(DEBUG_CFLAGS)
debug: $(LIB_STATIC) $(ENC_BIN) $(DEC_BIN) $(MUX_BIN) $(TEST_BIN)

# ── Build directory ──────────────────────────────────────────────

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ── Static library ───────────────────────────────────────────────

$(LIB_STATIC): $(ALL_OBJ) | $(BUILD_DIR)
	$(AR) rcs $@ $^

# ── Shared library ───────────────────────────────────────────────

shared: $(LIB_SHARED)

$(LIB_SHARED): $(ALL_OBJ) | $(BUILD_DIR)
	$(CC) -shared -o $@ $^ $(ARCH_LDFLAGS) $(LDFLAGS)

# ── CLI tools ────────────────────────────────────────────────────

$(ENC_BIN): $(TOOL_DIR)/tcenc.c $(LIB_STATIC) | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -o $@ $< $(LIB_STATIC) $(ARCH_LDFLAGS) $(LDFLAGS) -lm

$(DEC_BIN): $(TOOL_DIR)/tcdec.c $(LIB_STATIC) | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -o $@ $< $(LIB_STATIC) $(ARCH_LDFLAGS) $(LDFLAGS) -lm

$(MUX_BIN): $(TOOL_DIR)/tcmux.c | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -o $@ $< $(ARCH_LDFLAGS) $(LDFLAGS)

# ── Test binary ──────────────────────────────────────────────────

$(TEST_BIN): $(TEST_SRC) $(LIB_STATIC) | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -o $@ $< $(LIB_STATIC) $(ARCH_LDFLAGS) $(LDFLAGS) -lm

# The default regression is intentionally fast enough for constrained ARM hosts.
# It retains every unit test except the separately gated 300-frame 1080p soak.
test: test-fast

test-fast: test-common-deps
	ulimit -v 4000000 2>/dev/null; TCODEC_TEST_FAST=1 ./$(TEST_BIN)
	./tools/test_tcmux.sh
	sh ./tools/test_tcmux_mp4.sh

# Full in-process unit coverage, including test_long_run(). This can take
# substantially longer than the fast regression on Raspberry Pi-class hosts.
test-full: test-full-deps
	ulimit -v 4000000 2>/dev/null; ./$(TEST_BIN)
	./tools/test_tcmux.sh
	sh ./tools/test_tcmux_mp4.sh

test-common-deps: $(TEST_BIN) $(MUX_BIN) $(ENC_BIN) $(DEC_BIN)

test-full-deps: test-common-deps

test-mp4: $(MUX_BIN) $(ENC_BIN) $(DEC_BIN)
	sh ./tools/test_tcmux_mp4.sh

bench: $(ENC_BIN) $(DEC_BIN)
	@echo "Benchmark requires a YUV input file."
	@echo "Usage: ./$(ENC_BIN) -w 3840 -h 2160 -q 32 -v -o /dev/null input.yuv"

# ── Object files ─────────────────────────────────────────────────

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(NEON_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(TOOL_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -c -o $@ $<

# ── Install ──────────────────────────────────────────────────────

PREFIX ?= /usr/local

install: release
	install -d $(PREFIX)/lib $(PREFIX)/include/tcodec $(PREFIX)/bin
	install -m 644 $(LIB_STATIC) $(PREFIX)/lib/
	install -m 644 $(INC_DIR)/tcodec.h $(INC_DIR)/tcodec_types.h $(PREFIX)/include/tcodec/
	install -m 755 $(ENC_BIN) $(PREFIX)/bin/
	install -m 755 $(DEC_BIN) $(PREFIX)/bin/
	install -m 755 $(MUX_BIN) $(PREFIX)/bin/


uninstall:
	rm -f $(PREFIX)/lib/libtcodec.a
	rm -rf $(PREFIX)/include/tcodec
	rm -f $(PREFIX)/bin/tcenc $(PREFIX)/bin/tcdec $(PREFIX)/bin/tcmux

# ── Cross-compilation test for ARM (aarch64) ─────────────────────
#
# Compile-only test for the ARM NEON path.  Prefer clang-18 when the
# distro does not provide an unversioned clang; override CROSS_CC and
# CROSS_TARGET for another host/toolchain.
# Verifies that NEON guards are correct and all ARM sources compile.

CROSS_CC      ?= $(shell command -v clang-18 2>/dev/null || command -v clang 2>/dev/null || echo clang)
CROSS_TARGET  ?= aarch64-linux-gnu
CROSS_CFLAGS  = -target $(CROSS_TARGET) -DTCODEC_NEON=1 -D_GNU_SOURCE \
                -I$(INC_DIR) -std=c11 \
                -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare

.PHONY: cross-test cross-test-info

cross-test-info:
	@echo "Cross-compiling for ARM ($(CROSS_TARGET)) to verify NEON guards..."
	@echo "CC=$(CROSS_CC) CFLAGS=$(CROSS_CFLAGS)"

cross-test: cross-test-info
	@fail=0; \
	echo "── NEON sources ────────────────────────────────────"; \
	for f in $(NEON_DIR)/*.c; do \
	    echo -n "  $$f: "; \
	    $(CROSS_CC) $(CROSS_CFLAGS) -c "$$f" -o /dev/null 2>&1 && echo "OK" || { echo "FAIL"; fail=$$((fail+1)); }; \
	done; \
	echo "── Core sources (with TCODEC_NEON=1) ────────────────"; \
	for f in $(SRC_DIR)/*.c; do \
	    echo -n "  $$f: "; \
	    $(CROSS_CC) $(CROSS_CFLAGS) -c "$$f" -o /dev/null 2>&1 && echo "OK" || { echo "FAIL"; fail=$$((fail+1)); }; \
	done; \
	echo "── Tools + test (with TCODEC_NEON=1) ───────────────"; \
	for f in $(TOOL_DIR)/tcenc.c $(TOOL_DIR)/tcdec.c $(TOOL_DIR)/tcmux.c $(TEST_DIR)/test_tcodec.c; do \
	    echo -n "  $$f: "; \
	    $(CROSS_CC) $(CROSS_CFLAGS) -c "$$f" -o /dev/null 2>&1 && echo "OK" || { echo "FAIL"; fail=$$((fail+1)); }; \
	done; \
	echo "── NEON + no-threads (TCODEC_NEON=1 + TCODEC_NO_THREADS) ──"; \
	for f in $(SRC_DIR)/*.c $(NEON_DIR)/*.c $(TOOL_DIR)/tcenc.c $(TOOL_DIR)/tcdec.c $(TOOL_DIR)/tcmux.c $(TEST_DIR)/test_tcodec.c; do \
	    echo -n "  $$f: "; \
	    $(CROSS_CC) $(CROSS_CFLAGS) -DTCODEC_NO_THREADS -c "$$f" -o /dev/null 2>&1 && echo "OK" || { echo "FAIL"; fail=$$((fail+1)); }; \
	done; \
	if [ "$$fail" -ne 0 ]; then echo "$$fail file(s) FAILED cross-compilation!"; exit 1; \
	else echo "All cross-compilation tests passed."; fi

# ── No-threads build (for systems without pthread) ────────────────

# Note: requires clean build — run 'make clean && make nothreads' if objects
# from a normal build exist, since .o files won't be recompiled automatically.
nothreads: CFLAGS := $(CFLAGS) -DTCODEC_NO_THREADS
nothreads: ARCH_LDFLAGS :=
nothreads: clean $(LIB_STATIC) $(ENC_BIN) $(DEC_BIN) $(MUX_BIN)
	@echo "Built without threading support (TCODEC_NO_THREADS)"

nothreads-test: CFLAGS := $(CFLAGS) -DTCODEC_NO_THREADS
nothreads-test: ARCH_LDFLAGS :=
nothreads-test: clean $(TEST_BIN)
	./$(TEST_BIN)

# ── Clean ─────────────────────────────────────────────────────────

clean:
	rm -rf $(BUILD_DIR)

# ── Info ──────────────────────────────────────────────────────────

info:
	@echo "Architecture: $(UNAME_M)"
	@echo "NEON:         $(if $(NEON_SRC),YES,NO)"
	@echo "CC:           $(CC)"
	@echo "CFLAGS:       $(COMMON_CFLAGS)"
	@echo "Sources:      $(CORE_SRC) $(NEON_SRC)"
