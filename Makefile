CC = cc
CFLAGS = -std=c99 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -g
LDFLAGS =

BIN_DAEMON = simpled
BIN_CTL = simplectl

SRC_COMMON = src/util.c src/config.c
OBJ_COMMON = $(SRC_COMMON:.c=.o)

all: $(BIN_DAEMON) $(BIN_CTL)

$(BIN_DAEMON): src/simpled.o $(OBJ_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_CTL): src/simplectl.o $(OBJ_COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o $(BIN_DAEMON) $(BIN_CTL)

.PHONY: all clean
