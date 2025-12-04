CC = gcc
CFLAGS = -Wall -Wextra $(shell pkg-config --cflags libinput libudev)
LDFLAGS = $(shell pkg-config --libs libinput libudev)
TARGET = precision_backend

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c $(LDFLAGS) -lm

clean:
	rm -f $(TARGET)
