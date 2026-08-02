CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE -Wno-format-truncation
LDFLAGS := -lncursesw -lpthread -lm -ldl

SRC_DIR := src
BIN     := ldmp
SOURCES := $(SRC_DIR)/main.c $(SRC_DIR)/audio.c $(SRC_DIR)/library.c $(SRC_DIR)/playlist.c $(SRC_DIR)/fft.c $(SRC_DIR)/miniaudio_impl.c

# miniaudio's implementation file is large; keep -O1 for it so a full
# rebuild stays fast without hurting playback performance.
MINIAUDIO_OBJ := $(SRC_DIR)/miniaudio_impl.o

.PHONY: all clean run

all: $(BIN)

$(MINIAUDIO_OBJ): $(SRC_DIR)/miniaudio_impl.c $(SRC_DIR)/miniaudio.h
	$(CC) -O1 -std=c11 -D_GNU_SOURCE -c $< -o $@

$(BIN): $(SRC_DIR)/main.c $(SRC_DIR)/audio.c $(SRC_DIR)/library.c $(SRC_DIR)/playlist.c $(SRC_DIR)/fft.c $(MINIAUDIO_OBJ)
	$(CC) $(CFLAGS) $(SRC_DIR)/main.c $(SRC_DIR)/audio.c $(SRC_DIR)/library.c $(SRC_DIR)/playlist.c $(SRC_DIR)/fft.c $(MINIAUDIO_OBJ) -o $(BIN) $(LDFLAGS)

run: $(BIN)
	./$(BIN) $(ARGS)

clean:
	rm -f $(BIN) $(SRC_DIR)/*.o
