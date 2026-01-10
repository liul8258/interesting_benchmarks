CC = gcc
CFLAGS = -I/usr/local/include -Wall -O2
LDFLAGS = -L/usr/local/lib
LDLIBS = -lxdp -lbpf -lelf -lz

TARGET = xdp_client
SRC = xdp_client.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
        $(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
        rm -f $(TARGET)

# Helper to run with the correct library path
run: $(TARGET)
        sudo LD_LIBRARY_PATH=/usr/local/lib:$$LD_LIBRARY_PATH ./$(TARGET)
