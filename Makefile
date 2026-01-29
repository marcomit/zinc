CC = clang
CFLAGS = -Wall -Wextra -O2 $(shell llvm-config --cflags)
LDFLAGS = $(shell llvm-config --ldflags --libs core)
TARGET = zinc
SRC = zinc.c zmem.c zparse.c zlex.c zmod.c zsem.c zmacro.c zgen.c
INSTALL_DIR = $(HOME)/scripts

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

install: $(TARGET)
	make
	mkdir -p $(INSTALL_DIR)
	cp $(TARGET) $(INSTALL_DIR)/$(TARGET)
	chmod +x $(INSTALL_DIR)/$(TARGET)

test: $(TARGET)
	make install
	zinc tests/MathUtil.zn

clean:
	rm -f $(TARGET)

uninstall:
	rm -f $(INSTALL_DIR)/$(TARGET)

.PHONY: all install clean uninstall
