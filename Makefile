CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
OPT     ?=
LDFLAGS  = -lm -lpthread -rdynamic

SRC = main.c util.c config.c hardware.c command.c profile.c osd.c \
      keyframe.c rssi_monitor.c tx_monitor.c message.c cmd_server.c fallback.c
OBJ = $(SRC:.c=.o)
BIN = alink_drone

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) $(OPT) $(LDFLAGS) -o $@

%.o: %.c Makefile
	$(CC) -c $< $(CFLAGS) $(OPT) -o $@

clean:
	rm -f $(OBJ) $(BIN)
