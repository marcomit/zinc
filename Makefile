CC = clang
CXX = clang++
SHELL = bash

ifeq ($(OS), Windows_NT)
  UNAME := Windows
else
  UNAME := $(shell uname)
endif

ifeq ($(UNAME), Darwin)
  # brew install llvm ships LLD tools+libs but NOT the embedding API headers.
  # Those come from the standalone lld formula (brew install lld).
  LLD_PREFIX := $(firstword $(wildcard \
    /opt/homebrew/opt/lld \
    /opt/homebrew/opt/lld@20 \
    /opt/homebrew/opt/lld@19 \
    /opt/homebrew/opt/lld@18 \
  ))
  ifeq ($(LLD_PREFIX),)
    $(error LLD not found. Run: brew install lld)
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
  _LLVM_CFLAGS  := $(shell llvm-config --cflags 2>/dev/null)
  _LLVM_LDFLAGS := $(shell llvm-config --ldflags --libs core 2>/dev/null)
  _WIN_DEFS     :=
endif

CFLAGS   = -g -Wall -Wextra -Wdeprecated-declarations -O2 $(_WIN_DEFS) $(_LLVM_CFLAGS)
CXXFLAGS = -g -O2 -std=c++17 $(_WIN_DEFS) $(_LLVM_CFLAGS) $(LLD_INCLUDES)
LDFLAGS  = $(LLD_LIBS) $(_LLVM_LDFLAGS)
TARGET = zinc
BUILD_DIR = build

C_SRC = zinc.c zmem.c zparse.c zlex.c zmod.c zsem.c zmacro.c zgen.c
CXX_SRC = zlink.cpp
C_OBJ = $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRC))
CXX_OBJ = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(CXX_SRC))
OBJ = $(C_OBJ) $(CXX_OBJ)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) -o $(TARGET) $(OBJ) $(LDFLAGS)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

test: $(TARGET)
	make
	./run_tests.sh

clean:
	rm -f $(TARGET)
	rm -rf $(BUILD_DIR)

.PHONY: all clean
