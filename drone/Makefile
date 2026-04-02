# Adaptive-Link Drone Daemon Build System
# Located in: drone/Makefile

CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
OPT     ?=
LDFLAGS  = -lm -lpthread -rdynamic

SRC_DIR = src
SRC = main.c util.c config.c hardware.c command.c http_client.c profile.c osd.c \
      keyframe.c rssi_monitor.c tx_monitor.c message.c fallback.c
OBJ = $(addprefix $(SRC_DIR)/, $(SRC:.c=.o))
BIN = alink_drone

# Unity test framework
UNITY_DIR = test/unity
UNITY_SRC = $(UNITY_DIR)/src/unity.c
UNITY_INC = $(UNITY_DIR)/src

# Test sources
TEST_SRC = test/test_util.c
TEST_BIN = test/test_util

.PHONY: all clean test test-util

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) $(OPT) $(LDFLAGS) -o $@

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c Makefile
	$(CC) -c $< $(CFLAGS) $(OPT) -o $@

clean:
	rm -f $(OBJ) $(BIN)
	rm -f $(TEST_BIN)

# Test targets
test: $(TEST_BIN)
	./$(TEST_BIN)

test-util: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) $(SRC_DIR)/util.c $(UNITY_SRC)
	$(CC) -I$(SRC_DIR) -I$(UNITY_INC) $(CFLAGS) $(OPT) $(TEST_SRC) $(SRC_DIR)/util.c $(UNITY_SRC) \
		$(LDFLAGS) -ldl -o $@