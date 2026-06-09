CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -g

TARGET = aesdsocket
SRC = aesdsocket.c

.PHONY: all default clean

all: $(TARGET)

default: all

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET) *.o
