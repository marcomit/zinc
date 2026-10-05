CC = clang
CXX = clang++
SHELL = bash

ifeq ($(OS), Windows_NT)
  UNAME := Windows
else
  UNAME := $(shell uname)
endif

# Every machine (local and CI) builds against the same LLVM major, so the code
# never has to care about old vs new C API. Bump it here and in
# .github/actions/setup-llvm/action.yml together.
LLVM_VERSION ?= 22

# $(call llvm_major,cmd) - the major version printed by `cmd`, e.g. "22".
llvm_major = $(firstword $(subst ., ,$(lastword $(shell $(1) 2>/dev/null | head -1))))

ifneq ($(OS), Windows_NT)
  # First llvm-config whose major is LLVM_VERSION: versioned installs before
  # unversioned ones, so a newer default LLVM on the machine is skipped.
  LLVM_CONFIG ?= $(firstword $(foreach c, \
    /opt/homebrew/opt/llvm@$(LLVM_VERSION)/bin/llvm-config \
    /usr/local/opt/llvm@$(LLVM_VERSION)/bin/llvm-config \
    /usr/lib/llvm-$(LLVM_VERSION)/bin/llvm-config \
    llvm-config-$(LLVM_VERSION) \
    /opt/homebrew/opt/llvm/bin/llvm-config \
    /usr/local/opt/llvm/bin/llvm-config \
    llvm-config, \
    $(if $(filter $(LLVM_VERSION),$(call llvm_major,$(c) --version)),$(c))))
  ifeq ($(LLVM_CONFIG),)
    $(error LLVM $(LLVM_VERSION) not found. macOS: brew install llvm@$(LLVM_VERSION) lld@$(LLVM_VERSION) - Linux: see .github/actions/setup-llvm/action.yml)
  endif
endif

ifeq ($(UNAME), Darwin)
  # brew install llvm ships LLD tools+libs but NOT the embedding API headers.
  # Those come from the standalone lld formula (brew install lld).
  LLD_PREFIX := $(firstword $(foreach p, \
    /opt/homebrew/opt/lld@$(LLVM_VERSION) \
    /usr/local/opt/lld@$(LLVM_VERSION) \
    /opt/homebrew/opt/lld \
    /usr/local/opt/lld, \
    $(if $(filter $(LLVM_VERSION),$(call llvm_major,$(p)/bin/ld64.lld --version)),$(p))))
  ifeq ($(LLD_PREFIX),)
    $(error LLD $(LLVM_VERSION) not found. Run: brew install lld@$(LLVM_VERSION))
  endif
  LLD_INCLUDES := -I$(LLD_PREFIX)/include
  LLD_LIBS     := -L$(LLD_PREFIX)/lib -Wl,-rpath,$(LLD_PREFIX)/lib -llldMachO -llldCommon
else ifeq ($(UNAME), Windows)
  LLD_INCLUDES :=
  LLD_LIBS     := -llldCOFF -llldCommon
else
  LLD_INCLUDES :=
  LLD_LIBS     := -llldELF -llldCommon -lz -lzstd
endif

ifeq ($(UNAME), Windows)
  # $(shell llvm-config ...) doesn't work on Windows even with SHELL=bash because
  # make's subshell doesn't inherit the runner PATH. LLVM_CFLAGS / LLVM_LDFLAGS
  # are exported by a dedicated CI step that runs llvm-config in real bash.
  _LLVM_CFLAGS  := $(LLVM_CFLAGS)
  _LLVM_LDFLAGS := $(LLVM_LDFLAGS)
  # Suppress UCRT "use fopen_s / strncpy_s" noise; clang's own deprecated
  # attribute still fires for zinc's internal APIs.
  _WIN_DEFS     := -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS
else
  _LLVM_CFLAGS  := $(shell $(LLVM_CONFIG) --cflags 2>/dev/null)
  _LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --libs core 2>/dev/null)
  # libclang (the C API in clang-c/Index.h) is used by ctrans.c to read C headers.
  # It's a separate dylib, not one of llvm-config's components.
  _LLVM_LIBDIR  := $(shell $(LLVM_CONFIG) --libdir 2>/dev/null)
  _LLVM_LDFLAGS += -lclang -Wl,-rpath,$(_LLVM_LIBDIR)
  _WIN_DEFS     :=
endif

SRC_DIR = src
LIB_DIR = lib
INCLUDES = -I include -I lib

# Sanitizer flags. Empty for the normal build; the sanitizer presets below
# (debug / asan / ubsan) set this via a recursive make so instrumented and
# plain object files never end up in the same build dir.
SANITIZE ?=

# Set to -DZMODE=ZMODE_LSP by the `lsp` target; empty means the compiler (zinc.h).
ZMODE_FLAGS ?=

CFLAGS   = -g -Wall -Wextra -Wdeprecated-declarations -O2 $(SANITIZE) $(_WIN_DEFS) $(INCLUDES) $(_LLVM_CFLAGS) $(ZMODE_FLAGS)
CXXFLAGS = -g -O2 -std=c++17 $(SANITIZE) $(_WIN_DEFS) $(INCLUDES) $(_LLVM_CFLAGS) $(LLD_INCLUDES)
LDFLAGS  = $(SANITIZE) $(LLD_LIBS) $(_LLVM_LDFLAGS)
TARGET    = zinc
BUILD_DIR ?= build

# $(call rwildcard,dir,*.c) - like $(wildcard), but walks subdirectories too,
# so src/codegen/*.c and anything added later is picked up without listing it.
rwildcard = $(foreach d,$(wildcard $(1)/*),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))

# The language server is a separate binary with its own main(): its sources
# (and the JSON library only it uses) are kept out of the compiler build.
LSP_TARGET = zinc-lsp
LSP_DIR    = $(SRC_DIR)/lsp
JSON_DIR   = $(LIB_DIR)/json.c
LSP_EXCL   = $(LSP_DIR)/% $(JSON_DIR) $(JSON_DIR)/%

# Object files mirror the source tree: src/codegen/zgen.c -> build/codegen/zgen.o
C_SRC   = $(filter-out $(LSP_EXCL),$(call rwildcard,$(SRC_DIR),*.c) $(call rwildcard,$(LIB_DIR),*.c))
CXX_SRC = $(call rwildcard,$(SRC_DIR),*.cpp)
C_OBJ   = $(patsubst $(SRC_DIR)/%.c,  $(BUILD_DIR)/%.o, $(filter $(SRC_DIR)/%,$(C_SRC))) \
          $(patsubst $(LIB_DIR)/%.c,  $(BUILD_DIR)/%.o, $(filter $(LIB_DIR)/%,$(C_SRC)))
CXX_OBJ = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o, $(CXX_SRC))
OBJ     = $(C_OBJ) $(CXX_OBJ)

LSP_SRC = $(wildcard $(LSP_DIR)/*.c) $(JSON_DIR)/json.c
LSP_OBJ = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o, $(filter $(SRC_DIR)/%,$(LSP_SRC))) \
          $(patsubst $(LIB_DIR)/%.c,$(BUILD_DIR)/%.o, $(filter $(LIB_DIR)/%,$(LSP_SRC)))

all: $(TARGET)

# Build the language server. It is compiled with ZMODE=ZMODE_LSP (see zinc.h),
# which strips the CLI/codegen-only state, so it can't reuse the compiler's
# objects: the recursive make rebuilds the frontend into its own dir. The CLI
# (zcli.c, zinc.c with its main()) and the code generator are left out.
LSP_BUILD_DIR = build/lsp
LSP_FRONTEND  = $(filter-out $(SRC_DIR)/zinc.c $(SRC_DIR)/zcli.c $(SRC_DIR)/codegen/%,$(C_SRC))
LSP_DEPS      = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o, $(filter $(SRC_DIR)/%,$(LSP_FRONTEND))) \
                $(patsubst $(LIB_DIR)/%.c,$(BUILD_DIR)/%.o, $(filter $(LIB_DIR)/%,$(LSP_FRONTEND)))
# No LLD and no LLVM core: only libclang, which ctrans.c uses to read C headers.
LSP_LDFLAGS   = $(SANITIZE) -L$(_LLVM_LIBDIR) -lclang -Wl,-rpath,$(_LLVM_LIBDIR)

lsp:
	+$(MAKE) $(LSP_TARGET) BUILD_DIR=$(LSP_BUILD_DIR) ZMODE_FLAGS=-DZMODE=ZMODE_LSP

$(LSP_TARGET): $(LSP_OBJ) $(LSP_DEPS)
	$(CC) -o $@ $(LSP_OBJ) $(LSP_DEPS) $(LSP_LDFLAGS)

# --- Sanitizer builds ---------------------------------------------------
# Several sanitizers are mutually exclusive (ASan/TSan/MSan cannot be combined)
# and instrumented objects must never be linked against plain ones, so each
# preset rebuilds into its own dir and produces its own binary.
#
# Omitted on purpose:
#   memory  - reports false positives unless every linked lib (LLVM/LLD) is
#             also MSan-instrumented; Homebrew's prebuilt LLVM is not.
#   thread  - zinc is single-threaded, nothing to race.
#   fuzzer  - needs an LLVMFuzzerTestOneInput harness; zinc only has main().
#   cfi     - requires -flto and -fvisibility=hidden across the whole build.

ifeq ($(UNAME), Windows)
debug asan ubsan ubsan-int:
	@echo "sanitizer builds are not configured for Windows"
else
# ASan + UBSan: the default debug build. Best general-purpose catcher for
# memory errors (use-after-free, OOB) and undefined behavior.
debug asan:
	+$(MAKE) all BUILD_DIR=build/asan TARGET=$(TARGET)-asan \
	  SANITIZE="-fsanitize=address,undefined"

# UBSan limited to genuine undefined behavior (signed overflow, bad shifts,
# divide-by-zero, OOB, null deref, ...) plus local array bounds. These flag
# real bugs and stay green on correct code, so CI gates on this one.
ubsan:
	+$(MAKE) all BUILD_DIR=build/ubsan TARGET=$(TARGET)-ubsan \
	  SANITIZE="-fsanitize=undefined,local-bounds"

# Opt-in audit build: adds the integer / implicit-conversion checks. Those also
# fire on perfectly legal code (FNV hashing wraps on purpose, narrowing casts),
# so this is a manual auditing tool and is intentionally NOT wired into CI.
ubsan-int:
	+$(MAKE) all BUILD_DIR=build/ubsan-int TARGET=$(TARGET)-ubsan-int \
	  SANITIZE="-fsanitize=undefined,integer,implicit-conversion,local-bounds"
endif

$(TARGET): $(OBJ)
	$(CXX) -o $(TARGET) $(OBJ) $(LDFLAGS)

# Like the $(TARGET) rule, but links *every* object under $(BUILD_DIR) instead of
# just $(OBJ) - so extra objects you drop in that aren't built from src/ (e.g. a
# Zinc-compiled build/compiler.o) get linked too. $(OBJ) as a prerequisite builds
# the compiler's own sources first; find then sweeps them up with the extras. The
# sanitizer subdirs are pruned - instrumented objects must never mix with plain ones.
link: $(OBJ)
	$(CXX) -o $(TARGET) \
	  $$(find $(BUILD_DIR) -type d \( -name asan -o -name ubsan -o -name ubsan-int \) -prune \
	     -o -type f -name '*.o' -print) \
	  $(LDFLAGS)

# The % stem spans slashes, so these also match src/codegen/zgen.c. Each recipe
# creates its own output dir because the nested ones don't exist up front.
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(LIB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test: $(TARGET)
	make
	./run_tests.sh

# Install the std library inside the global package registry
install: $(TARGET)
ifeq ($(UNAME), Windows)
	@registry="$$(cygpath -u "$$USERPROFILE")/.zinc/packages/std"; \
	  rm -rf "$$registry" && mkdir -p "$$registry" && cp -R std/. "$$registry/"; \
	  echo "installed stdlib -> $$registry"
else
	@registry="$$HOME/.zinc/packages/std"; \
	  rm -rf "$$registry" && mkdir -p "$$registry" && cp -R std/. "$$registry/"; \
	  echo "installed stdlib -> $$registry"
endif

# Count lines of code (zinc.cloc teaches cloc about .zn files)
loc:
	@cloc --read-lang-def=zinc.cloc --include-ext=c,h,zn,def .

clean:
	rm -f $(TARGET) $(LSP_TARGET) $(TARGET)-asan $(TARGET)-ubsan $(TARGET)-ubsan-int
	rm -rf build

.PHONY: all lsp link debug asan ubsan ubsan-int clean install test loc
