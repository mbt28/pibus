# Build configuration for Raspberry Pi 5 (64-bit)
CC     = aarch64-linux-gnu-gcc
STRIP  = aarch64-linux-gnu-strip

CFLAGS  = -Wall -O2 -ggdb
LDFLAGS =
LDLIBS  = -lgpiod -lrt

SRC = mainloop.c slist.c pibus.c ibus.c ibus-send.c keyboard.c gpio.c server.c log.c annotate.c
OBJ = $(SRC:.c=.o)
TARGET = pibus

all: $(TARGET) strip

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

strip: $(TARGET)
	cp $(TARGET) $(TARGET).debug
	$(STRIP) -R .comment $(TARGET)

clean:
	rm -f $(OBJ) $(TARGET) $(TARGET).debug

.PHONY: all strip clean
