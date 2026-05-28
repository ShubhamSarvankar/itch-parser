# Convenience Makefile around the cmake build.
# cmake is still the source of truth; this just gives short targets.

BUILD_DIR ?= build
BUILD_TYPE ?= Release
ITCH_FILE  ?= data/12302019.NASDAQ_ITCH50
SNAP       ?= 1000

PERF_EVENTS = cycles,instructions,cache-references,cache-misses,\
branch-instructions,branch-misses,L1-dcache-loads,L1-dcache-load-misses,\
LLC-loads,LLC-load-misses,dTLB-load-misses

.PHONY: configure build replay perf-baseline bench bench-replay perf-bench clean

configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	cmake --build $(BUILD_DIR) -j

replay: build
	@mkdir -p bench/out
	./$(BUILD_DIR)/bench/bench_replay $(ITCH_FILE) $(SNAP) | tee bench/out/replay.log

perf-baseline: build
	@mkdir -p bench/out
	perf stat -e $(PERF_EVENTS) -o bench/out/baseline.perf -- \
		./$(BUILD_DIR)/bench/bench_replay $(ITCH_FILE) $(SNAP) \
		| tee bench/out/baseline.log
	@echo "wrote bench/out/baseline.perf  bench/out/baseline.log"

bench: build
	./$(BUILD_DIR)/bench/itch_bench --benchmark_out=bench/out/bench.json \
		--benchmark_out_format=json

bench-replay: build
	@mkdir -p bench/out
	ITCH_PROFILE=1 ./$(BUILD_DIR)/bench/bench_replay $(ITCH_FILE) $(SNAP) \
		| tee bench/out/bench_replay.log

perf-bench: build
	@mkdir -p bench/out
	perf stat -e $(PERF_EVENTS) -o bench/out/bench.perf -- \
		./$(BUILD_DIR)/bench/itch_bench --benchmark_min_time=2s \
		| tee bench/out/bench.log

clean:
	rm -rf $(BUILD_DIR)
