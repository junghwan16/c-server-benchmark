# High-Performance C HTTP Server Benchmark
PROJECT := c-server-benchmark
BUILD   := build
CC      := clang
CFLAGS  := -Wall -Wextra -Werror -O2 -std=c11 -D_XOPEN_SOURCE=700
LDFLAGS := 
THREAD_LIB := -lpthread

SRC_COMMON   := src/common/http.c src/common/util.c
SRC_AIO      := src/aio_srv/aio_server.c $(SRC_COMMON) src/main_aio.c
SRC_THREAD   := src/thread_srv/thread_pool_server.c $(SRC_COMMON) src/main_thread.c
SRC_KQUEUE   := src/kqueue_srv/kqueue_server.c $(SRC_COMMON) src/main_kqueue.c

OBJ_AIO      := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC_AIO))
OBJ_THREAD   := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC_THREAD))
OBJ_KQUEUE   := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC_KQUEUE))

BIN_AIO      := $(BUILD)/aio_http
BIN_THREAD   := $(BUILD)/thread_http
BIN_KQUEUE   := $(BUILD)/kqueue_http

.PHONY: all clean run-aio run-thread run-kqueue bench

all: $(BIN_AIO) $(BIN_THREAD) $(BIN_KQUEUE)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_AIO): $(OBJ_AIO)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_THREAD): $(OBJ_THREAD)
	$(CC) $(CFLAGS) $^ -o $@ $(THREAD_LIB) $(LDFLAGS)

$(BIN_KQUEUE): $(OBJ_KQUEUE)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run-aio: $(BIN_AIO)
	./$(BIN_AIO)

run-thread: $(BIN_THREAD)
	./$(BIN_THREAD)

run-kqueue: $(BIN_KQUEUE)
	./$(BIN_KQUEUE)

bench:
	bash scripts/bench.sh

clean:
	rm -rf $(BUILD)
