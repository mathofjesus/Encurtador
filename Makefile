CC      ?= gcc
# -Isrc lets sources include "util/buf.h" rather than "../../util/buf.h".
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g -D_GNU_SOURCE -Isrc
LDFLAGS ?=

# Debian installs libpq's headers under /usr/include/postgresql, so -lpq alone
# links but cannot compile. Ask pkg-config where they are, and keep the explicit
# -l flags as the fallback for a build without it.
PKG_CFLAGS  := $(shell pkg-config --cflags libpq 2>/dev/null)
APP_LIBS    := -lpq -lhiredis -lpthread -lm
TEST_LIBS   := -lcriterion $(APP_LIBS)
ALL_CFLAGS  := $(CFLAGS) $(PKG_CFLAGS)

SRC_DIR   := src
TEST_DIR  := tests
BUILD_DIR := build
BIN_DIR   := bin
TARGET    := $(BIN_DIR)/shortener

MAIN_SRC := $(SRC_DIR)/main.c
LIB_SRC  := $(shell find $(SRC_DIR) -name '*.c' ! -name 'main.c' 2>/dev/null)
LIB_OBJ  := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(LIB_SRC))
TEST_SRC := $(shell find $(TEST_DIR) -name '*.c' ! -name 'test_disk_full.c' 2>/dev/null)
TEST_BIN := $(patsubst $(TEST_DIR)/%.c,$(BUILD_DIR)/%,$(TEST_SRC))

# Excluded from `make test` on purpose: it fills the database until Postgres
# refuses to write, after which every write in the instance fails. Run only by
# `make test-disk`, against a small tmpfs, in an instance of its own.
TEST_DISK_SRC := $(TEST_DIR)/test_disk_full.c
TEST_DISK_BIN := $(patsubst $(TEST_DIR)/%.c,$(BUILD_DIR)/%,$(TEST_DISK_SRC))

# -fno-sanitize-recover=all makes UBSan abort on the first report instead of
# printing and continuing. Without it a runtime error prints a line and the run
# still exits 0, which is how a one-byte body overflow passed as "SANITIZED
# TESTS PASSED" once already.
ASAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all \
              -fno-omit-frame-pointer

.PHONY: all build test test-asan test-e2e test-disk bench clean

all: build

build: $(TARGET)

$(TARGET): $(MAIN_SRC) $(LIB_OBJ) | $(BIN_DIR)
	$(CC) $(ALL_CFLAGS) $(MAIN_SRC) $(LIB_OBJ) -o $@ $(LDFLAGS) $(APP_LIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

# Each test file links the library objects plus Criterion.
$(BUILD_DIR)/%: $(TEST_DIR)/%.c $(LIB_OBJ) | $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) $< $(LIB_OBJ) -o $@ $(LDFLAGS) $(TEST_LIBS)

# Criterion runs each test in its own process, in parallel by default. Two
# reasons these binaries are serial:
#   - test_pg / test_redis / test_url_service / test_router / test_e2e share one
#     Postgres and one Redis, and the last three drive both. In parallel they
#     race on the same DDL (CREATE TABLE IF NOT EXISTS still loses that race —
#     SQLSTATE 42P01 and duplicate pg_class_relname_nsp_index), and a process per
#     test at pool-of-2 blows past Postgres's max_connections of 20.
#   - test_reactor is timing-sensitive. Several processes polling on the same
#     0.35-core share starve each other, and a poll budget that is generous alone
#     becomes too short.
TEST_SHARED := build/test_pg build/test_redis build/test_url_service \
               build/test_router build/test_e2e build/test_reactor

test: $(TEST_BIN)
	@if [ -z "$(strip $(TEST_BIN))" ]; then \
	  echo "no tests found under $(TEST_DIR)/ -- a green run with zero tests is not a pass"; \
	  exit 1; \
	fi
	@fail=0; for t in $(TEST_BIN); do \
	  echo "=== $$t"; \
	  case " $(TEST_SHARED) " in \
	    *" $$t "*) $$t -j1 2>&1 || fail=1 ;; \
	    *)            $$t 2>&1 || fail=1 ;; \
	  esac; \
	done; \
	if [ $$fail -eq 0 ]; then echo "ALL TESTS PASSED"; fi; \
	exit $$fail

# ASan + UBSan build of the whole suite (Task 1 leak pass, Task 2 fuzz pass).
test-asan:
	@mkdir -p $(BUILD_DIR)/asan
	@fail=0; for src in $(TEST_SRC); do \
	  name=`basename $$src .c`; \
	  out=$(BUILD_DIR)/asan/$$name; \
	  echo "=== $$out (asan+ubsan)"; \
	  $(CC) $(ALL_CFLAGS) $(ASAN_FLAGS) $$src $(LIB_SRC) -o $$out $(LDFLAGS) $(TEST_LIBS) || { fail=1; continue; }; \
	  case " $(TEST_SHARED) " in \
	    *" $(BUILD_DIR)/$$name "*) $$out -j1 2>&1 || fail=1 ;; \
	    *)                        $$out 2>&1 || fail=1 ;; \
	  esac; \
	done; \
	if [ $$fail -eq 0 ]; then echo "ALL SANITIZED TESTS PASSED"; fi; \
	exit $$fail

test-e2e: build
	./$(TARGET) --self-test

# Runs in place, inside whatever sandbox the caller set up. It must not call
# scripts/sandbox.sh: that script runs `make test-disk` inside the container, so
# a version of this target that shelled back out would recurse forever.
test-disk: $(TEST_DISK_BIN)
	@if [ -z "$(strip $(TEST_DISK_BIN))" ]; then \
	  echo "test_disk_full.c is missing; this target would be vacuous"; \
	  exit 1; \
	fi
	$(TEST_DISK_BIN) -j1

bench: build
	./scripts/sandbox.sh bench

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
