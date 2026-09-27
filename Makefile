# Convenience wrapper around CMake
BUILD_DIR ?= build
BUILD_TYPE ?= Release

.PHONY: all configure build clean test install

all: build

configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	cmake --build $(BUILD_DIR) -j
	@echo "Built: $(BUILD_DIR)/simplec"

clean:
	rm -rf $(BUILD_DIR)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

install: build
	cmake --install $(BUILD_DIR)
