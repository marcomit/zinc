CC = clang
CXX = clang++
UNAME := $(shell uname)

ifeq ($(UNAME), Darwin)
  LLD_PREFIX := $(or $(wildcard /opt/homebrew/opt/lld),$(wildcard /opt/homebrew/opt/lld@19))
  LLD_INCLUDES := -I$(LLD_PREFIX)/include
  LLD_LIBS := -L$(LLD_PREFIX)/lib -Wl,-rpath,$(LLD_PREFIX)/lib -llldMachO -llldCommon
else
  LLD_INCLUDES :=
  LLD_LIBS := -llldELF -llldCommon -lz -lzstd
endif

CFLAGS = -g -Wall -Wextra -O2 $(shell llvm-config --cflags)
CXXFLAGS = -g -O2 -std=c++17 $(shell llvm-config --cflags) $(LLD_INCLUDES)
LDFLAGS = $(shell llvm-config --ldflags --libs core) $(LLD_LIBS)
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
