# Convenience wrapper around the CMake presets (the real build system is CMake;
# see CMakePresets.json). Usage: `make build`, `make test`, `make run`, ...
PRESET ?= release
BUILD  := build/$(PRESET)
CONFIG ?= configs/default.json

SOURCES := $(shell find include src apps tests -type f \( -name '*.hpp' -o -name '*.cpp' \))
TU      := $(shell find src apps -type f -name '*.cpp')

.PHONY: all configure build test run convert sample format tidy tidy-fix clean help

all: build

configure: ## Configure the CMake build (fetches deps on first run)
	cmake --preset $(PRESET)

build: configure ## Build the library, apps, and tests
	cmake --build --preset $(PRESET)

test: build ## Run the unit/integration test suite
	ctest --preset $(PRESET) --output-on-failure

convert: build ## Convert the sample CSVs to packed binary (one-time)
	$(BUILD)/convert_csv lob    market_data/lob.csv    market_data/lob.bin
	$(BUILD)/convert_csv trades market_data/trades.csv market_data/trades.bin

run: build ## Run a backtest (override with CONFIG=path/to.json)
	$(BUILD)/backtest $(CONFIG)

sample: build ## Convert the bundled sample data and backtest it (no large data needed)
	$(BUILD)/convert_csv lob    market_data/lob_sample.csv    market_data/lob_sample.bin
	$(BUILD)/convert_csv trades market_data/trades_sample.csv market_data/trades_sample.bin
	$(BUILD)/backtest configs/sample.json

format: ## Apply clang-format to all sources
	clang-format-21 -i $(SOURCES)

tidy: build ## Report clang-tidy findings
	clang-tidy-21 -p $(BUILD) $(TU)

tidy-fix: build ## Auto-apply clang-tidy fixes (e.g. braces) to the sources
	clang-tidy-21 -p $(BUILD) --fix --fix-errors $(TU)

clean: ## Remove the build directory
	rm -rf build

help: ## List targets
	@grep -hE '^[a-z-]+:.*##' $(MAKEFILE_LIST) | sed -E 's/:.*## / - /' | sort
