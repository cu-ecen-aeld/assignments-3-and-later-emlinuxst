CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -g
LDFLAGS ?=
USE_AESD_CHAR_DEVICE ?= 1

TARGET = aesdsocket
SRC = aesdsocket.c

.PHONY: all default clean

all: $(TARGET)

default: all

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -DUSE_AESD_CHAR_DEVICE=$(USE_AESD_CHAR_DEVICE) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o
