# Build configuration for Raspberry Pi 5 (64-bit)
CC     = aarch64-linux-gnu-gcc
STRIP  = aarch64-linux-gnu-strip

CFLAGS  = -Wall -O2 -ggdb
LDFLAGS =
LDLIBS  = -lrt   # optional; harmless even if not required

SRC = mainloop.c slist.c pibus.c ibus.c ibus-send.c keyboard.c gpio.c server.c log.c annotate.c
TARGET = pibus

all: $(TARGET) strip

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

strip: $(TARGET)
	cp $(TARGET) $(TARGET).debug
	$(STRIP) -R .comment $(TARGET)

clean:
	rm -f $(TARGET) $(TARGET).debug *.o

.PHONY: all strip clean
