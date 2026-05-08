# LOB Backtester

Event-driven C++20 backtest engine for historical limit-order-book replay and
market-making strategies.

## Build

Dependencies are fetched by CMake through `FetchContent`:

- `yaml-cpp`
- `spdlog`
- `googletest`

```bash
cmake -S lob_backtester -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

If `ctest` is not available in the shell, the same test binary can be run
directly:

```bash
./build/lob_tests
```

## Run

```bash
./build/lob_backtest --config lob_backtester/configs/example.yaml
```

The current CLI is a bootstrap smoke path: it parses the YAML file, logs that the
configuration was loaded, prints the resolved parameters, and exits with status 0.

## Documentation

- [Implementation plan](../docs/implementation_plan.md)
- [Technical documentation](../docs/technical_documentation.md)

## Format And Lint

```bash
find lob_backtester/apps lob_backtester/src lob_backtester/tests -name '*.cpp' -o -name '*.hpp' \
  | xargs clang-format --dry-run --Werror
clang-tidy -p build lob_backtester/apps/lob_backtest.cpp lob_backtester/src/lob/utils/Config.cpp
```

## Python Utilities

Python scripts for data audit, plotting, and report generation live under
`lob_backtester/scripts/python`.

```bash
python3.11 -m venv lob_backtester/scripts/python/.venv
source lob_backtester/scripts/python/.venv/bin/activate
pip install -r lob_backtester/scripts/python/requirements.txt
```

Run the raw data audit and regenerate the deterministic one-hour sample:

```bash
python3 lob_backtester/scripts/python/audit.py --json-out data/sample/audit_summary.json
```
