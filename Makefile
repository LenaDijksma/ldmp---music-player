CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE -Wno-format-truncation
LDFLAGS := -lncursesw -lpthread -lm -ldl

# MPRIS2 (org.mpris.MediaPlayer2) support lets desktop widgets / waybar
# / eww / ags "now playing" modules and media keys see and control
# ldmp. It's opt-in at build time based on whether libdbus-1-dev is
# available (dbus-1 itself is present on basically every Linux desktop
# at runtime; it's only the -dev headers that are sometimes missing).
DBUS_CFLAGS := $(shell pkg-config --cflags dbus-1 2>/dev/null)
DBUS_LIBS   := $(shell pkg-config --libs dbus-1 2>/dev/null)
ifneq ($(strip $(DBUS_LIBS)),)
    CFLAGS  += -DLDMP_MPRIS $(DBUS_CFLAGS)
    LDFLAGS += $(DBUS_LIBS)
endif

SRC_DIR := src
BIN     := ldmp
SOURCES := $(SRC_DIR)/main.c $(SRC_DIR)/audio.c $(SRC_DIR)/library.c $(SRC_DIR)/playlist.c $(SRC_DIR)/fft.c $(SRC_DIR)/mpris.c $(SRC_DIR)/miniaudio_impl.c

# miniaudio's implementation file is large; keep -O1 for it so a full
# rebuild stays fast without hurting playback performance.
MINIAUDIO_OBJ := $(SRC_DIR)/miniaudio_impl.o

.PHONY: all clean run

all: $(BIN)

$(MINIAUDIO_OBJ): $(SRC_DIR)/miniaudio_impl.c $(SRC_DIR)/miniaudio.h
	$(CC) -O1 -std=c11 -D_GNU_SOURCE -c $< -o $@

$(BIN): $(SRC_DIR)/main.c $(SRC_DIR)/audio.c $(SRC_DIR)/library.c $(SRC_DIR)/playlist.c $(SRC_DIR)/fft.c $(SRC_DIR)/mpris.c $(MINIAUDIO_OBJ)
	$(CC) $(CFLAGS) $(SRC_DIR)/main.c $(SRC_DIR)/audio.c $(SRC_DIR)/library.c $(SRC_DIR)/playlist.c $(SRC_DIR)/fft.c $(SRC_DIR)/mpris.c $(MINIAUDIO_OBJ) -o $(BIN) $(LDFLAGS)

run: $(BIN)
	./$(BIN) $(ARGS)

clean:
	rm -f $(BIN) $(SRC_DIR)/*.o
