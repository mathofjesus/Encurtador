CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g -D_GNU_SOURCE
LDFLAGS ?=

# The library sandbox installs the same set.
APP_LIBS    := -lpq -lhiredis -lpthread -lm
TEST_LIBS   := -lcriterion $(APP_LIBS)

SRC_DIR   := src
TEST_DIR  := tests
BUILD_DIR := build
BIN_DIR   := bin
TARGET    := $(BIN_DIR)/shortener

MAIN_SRC := $(SRC_DIR)/main.c
LIB_SRC  := $(shell find $(SRC_DIR) -name '*.c' ! -name 'main.c' 2>/dev/null)
LIB_OBJ  := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(LIB_SRC))
TEST_SRC := $(shell find $(TEST_DIR) -name '*.c' 2>/dev/null)
TEST_BIN := $(patsubst $(TEST_DIR)/%.c,$(BUILD_DIR)/%,$(TEST_SRC))

ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all build test test-asan test-e2e test-disk bench clean

all: build

build: $(TARGET)

$(TARGET): $(MAIN_SRC) $(LIB_OBJ) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(MAIN_SRC) $(LIB_OBJ) -o $@ $(LDFLAGS) $(APP_LIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

# Each test file links the library objects plus Criterion.
$(BUILD_DIR)/%: $(TEST_DIR)/%.c $(LIB_OBJ) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(LIB_OBJ) -o $@ $(LDFLAGS) $(TEST_LIBS)

test: $(TEST_BIN)
	@if [ -z "$(strip $(TEST_BIN))" ]; then \
	  echo "no tests found under $(TEST_DIR)/ -- a green run with zero tests is not a pass"; \
	  exit 1; \
	fi
	@fail=0; for t in $(TEST_BIN); do \
	  echo "=== $$t"; \
	  $$t 2>&1 || fail=1; \
	done; \
	if [ $$fail -eq 0 ]; then echo "ALL TESTS PASSED"; fi; \
	exit $$fail

# ASan + UBSan build of the whole suite (Task 1 leak pass, Task 2 fuzz pass).
test-asan:
	@mkdir -p $(BUILD_DIR)/asan
	@fail=0; for src in $(TEST_SRC); do \
	  out=$(BUILD_DIR)/asan/`basename $$src .c`; \
	  echo "=== $$out (asan+ubsan)"; \
	  $(CC) $(CFLAGS) $(ASAN_FLAGS) $$src $(LIB_SRC) -o $$out $(LDFLAGS) $(TEST_LIBS) || { fail=1; continue; }; \
	  $$out 2>&1 || fail=1; \
	done; \
	if [ $$fail -eq 0 ]; then echo "ALL SANITIZED TESTS PASSED"; fi; \
	exit $$fail

test-e2e: build
	./$(TARGET) --self-test

test-disk:
	./scripts/sandbox.sh test-disk

bench: build
	./scripts/sandbox.sh bench

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
