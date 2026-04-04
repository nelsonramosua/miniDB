# Makefile – kvstore build system
#
# Targets:
#   make            – build release binary
#   make debug      – build with AddressSanitizer + debug symbols
#   make test       – build and run unit tests
#   make valgrind   – run tests under valgrind (requires valgrind)
#   make benchmark  – run redis-benchmark against localhost:7380
#   make test-persistence – verify snapshot save/load across restart
#   make clean      – remove build artefacts

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic \
            -Wmissing-prototypes -Wstrict-prototypes \
            -Wshadow -Wno-unused-parameter
LDFLAGS :=

RELEASE_FLAGS := -O2 -DNDEBUG
DEBUG_FLAGS   := -O0 -g3 -fsanitize=address,undefined \
                  -fno-omit-frame-pointer

SRC_DIR   := src
TEST_DIR  := tests
BUILD_DIR := build
INCFLAGS  := -I$(SRC_DIR) -Iinclude -I.

SRCS := $(sort $(shell find $(SRC_DIR) -type f -name '*.c'))
# Exclude main.c from test builds
LIB_SRCS := $(filter-out $(SRC_DIR)/main.c, $(SRCS))
STYLE_FILES := $(sort $(shell find $(SRC_DIR) include $(TEST_DIR) -type f \( -name '*.c' -o -name '*.h' \)))
TIDY_FILES := $(sort $(SRCS) $(shell find $(TEST_DIR) -type f -name '*.c'))

OBJS         := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))
LIB_OBJS     := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(LIB_SRCS))

TARGET := kvstore
DEBUG_TARGET := kvstore_debug
TEST_STORE_BIN := test_store
TEST_PROTO_BIN := test_proto
TEST_PERSIST_BIN := test_persist

# ── Release ─────────────────────────────────────────────────────────────────

.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(RELEASE_FLAGS) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(RELEASE_FLAGS) $(INCFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# ── Debug (ASan + UBSan) ─────────────────────────────────────────────────────

.PHONY: debug
debug: CFLAGS += $(DEBUG_FLAGS)
debug: RELEASE_FLAGS :=
debug: $(DEBUG_TARGET)

$(DEBUG_TARGET): $(patsubst $(BUILD_DIR)/%.o, $(BUILD_DIR)/%_dbg.o, $(OBJS))
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%_dbg.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) $(INCFLAGS) -c -o $@ $<

# ── Tests ────────────────────────────────────────────────────────────────────

TEST_BINS := $(TEST_STORE_BIN) $(TEST_PROTO_BIN) $(TEST_PERSIST_BIN)

.PHONY: test
test: $(TEST_BINS)
	echo ""; echo "Running test_store..."; ./$(TEST_STORE_BIN)
	echo ""; echo "Running test_proto..."; ./$(TEST_PROTO_BIN)
	echo ""; echo "Running test_persist..."; ./$(TEST_PERSIST_BIN)

$(TEST_STORE_BIN): $(TEST_DIR)/testStore.c $(LIB_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(RELEASE_FLAGS) $(INCFLAGS) -o $@ $^

$(TEST_PROTO_BIN): $(TEST_DIR)/testProtocol.c $(LIB_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(RELEASE_FLAGS) $(INCFLAGS) -o $@ $^

$(TEST_PERSIST_BIN): $(TEST_DIR)/testPersist.c $(LIB_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(RELEASE_FLAGS) $(INCFLAGS) -o $@ $^

# ── Valgrind ─────────────────────────────────────────────────────────────────

.PHONY: valgrind
valgrind: $(TEST_BINS)
	valgrind --leak-check=full --error-exitcode=1 ./$(TEST_STORE_BIN)
	valgrind --leak-check=full --error-exitcode=1 ./$(TEST_PROTO_BIN)

.PHONY: benchmark
benchmark:
	./scripts/benchmark.sh 7380

.PHONY: test-persistence
test-persistence: $(TARGET)
	./scripts/test_persistence.sh 7390

.PHONY: format
format:
	@command -v clang-format >/dev/null 2>&1 || { echo "clang-format not installed"; exit 1; }
	@echo "Formatting C files with clang-format..."
	@clang-format -i $(STYLE_FILES)

.PHONY: lint-style
lint-style:
	@command -v clang-format >/dev/null 2>&1 || { echo "clang-format not installed"; exit 1; }
	@echo "Checking formatting with clang-format..."
	@clang-format --dry-run --Werror $(STYLE_FILES)
	@if command -v clang-tidy >/dev/null 2>&1; then \
		echo "Running naming checks with clang-tidy..."; \
		if [ -f compile_commands.json ]; then \
			clang-tidy $(TIDY_FILES) -p .; \
		else \
			clang-tidy $(TIDY_FILES) -- $(INCFLAGS); \
		fi; \
	else \
		echo "clang-tidy not installed; skipping naming checks."; \
	fi

# ── Housekeeping ─────────────────────────────────────────────────────────────

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(DEBUG_TARGET) $(TEST_STORE_BIN) $(TEST_PROTO_BIN) $(TEST_PERSIST_BIN)

# Ensure header changes trigger recompilation
$(BUILD_DIR)/%.o: $(wildcard include/*.h)