CC := cc
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -g -Iinclude
LDFLAGS := -Wl,-l:libncursesw.so.6 -Wl,-l:libtinfo.so.6

TARGET := sector_editor
SRC := \
	src/device.c \
	src/disk.c \
	src/ui.c \
	src/main.c

OBJ := $(SRC:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

src/%.o: src/%.c include/device.h include/disk.h include/ui.h include/editor.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
