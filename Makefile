# Convenience wrapper around the CMake presets (the real build system is CMake;
# see CMakePresets.json). Usage: `make build`, `make test`, `make run`, ...
PRESET ?= release
BUILD  := build/$(PRESET)
CONFIG ?= configs/default.json

SOURCES := $(shell find include src apps tests -type f \( -name '*.hpp' -o -name '*.cpp' \))
TU      := $(shell find src apps -type f -name '*.cpp')

.PHONY: all configure build test run convert sample experiments sweep viz-install plot plots format tidy tidy-fix clean help

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

experiments: build ## Run fixed / A-S / micro-price-A-S on the full data (-> reports/)
	@mkdir -p reports
	$(BUILD)/backtest configs/fixed.json
	$(BUILD)/backtest configs/as.json
	$(BUILD)/backtest configs/microprice_as.json
	$(BUILD)/backtest configs/as_online.json

sweep: build ## Run the gamma risk-aversion sweep on the full data (-> reports/gamma_sweep.csv)
	BUILD=$(BUILD) ./scripts/gamma_sweep.sh

sample: build ## Convert the bundled sample data and backtest it (no large data needed)
	$(BUILD)/convert_csv lob    market_data/lob_sample.csv    market_data/lob_sample.bin
	$(BUILD)/convert_csv trades market_data/trades_sample.csv market_data/trades_sample.bin
	$(BUILD)/backtest configs/sample.json

viz-install: ## Install the Python plotting project (viz/, via poetry)
	cd viz && poetry install

plot: sample ## Backtest the sample and render price/PnL/inventory/volume plots (needs viz-install)
	cd viz && poetry run bt-viz ../reports/series_sample.csv

plots: ## Render reports/series_<strategy>.png for every series CSV (run experiments first)
	@files=$$(ls reports/series_*.csv 2>/dev/null); \
	if [ -z "$$files" ]; then echo "no reports/series_*.csv — run 'make experiments' first"; exit 1; fi; \
	for f in $$files; do echo "plotting $$f"; (cd viz && poetry run bt-viz "../$$f"); done

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
