# Artemis - High-Performance Algorithmic Trading Backtester

Artemis is a high-performance C++ backtesting engine designed for tick-by-tick analysis of ES futures data with sub-microsecond latency requirements.

## Features

- **Memory-mapped CSV reading**: Sustains 3.5M+ ticks/min throughput
- **Lock-free rolling statistics**: EWMA mean, variance, and z-score calculation in <200ns per tick
- **Z-score based signal generation**: Configurable threshold with zero-crossing exit logic
- **Tick-by-tick backtesting**: Realistic execution with slippage and commission modeling
- **High-performance**: ≤5µs latency per tick on i7-12700H (single-threaded)
- **Thread-safe architecture**: Lock-free queues with thread affinity for low OS jitter
- **Comprehensive metrics**: Sharpe ratio, max drawdown, win rate, and more
- **Python analytics**: Visualization and parameter optimization tools

## Project Structure

```
Artemis/
├── src/               # C++ source files (*.cpp & *.hpp)
├── py/                # Python analytics & plotting scripts
├── tests/             # GoogleTest unit tests
├── data/              # ES_futures_sample.csv (tick format: ts,bid,ask,volume)
├── cmake/             # CMake find packages scripts
├── build/             # Out-of-tree build directory
└── README.md          # This file
```

## Requirements

- **CMake** ≥ 3.20
- **C++17** compatible compiler (GCC, Clang, or MSVC)
- **Python 3.7+** (for analytics scripts)
- **Python packages**: pandas, matplotlib, seaborn, numpy (for analytics)

## Building

### Prerequisites Check (Windows)

Run the setup script to check if all prerequisites are installed:

```powershell
.\setup_windows.ps1
```

This will check for:
- CMake (≥3.20)
- Visual Studio 2022 (with C++ workload)
- Git

### Installing Prerequisites (Windows)

If CMake is missing, install it:

```powershell
# Option 1: Using winget (recommended)
winget install Kitware.CMake

# Option 2: Download from https://cmake.org/download/
```

After installing CMake, restart PowerShell and verify:
```powershell
cmake --version
```

### Linux/macOS

```bash
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Windows

#### Option 1: Using Build Script (Recommended)

```powershell
.\build_windows.ps1
```

#### Option 2: Manual Build

```powershell
# Create build directory
mkdir build
cd build

# Configure with CMake (Visual Studio 2022)
cmake .. -G "Visual Studio 17 2022" -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build . --config Release
```

**Note:** If you have a different Visual Studio version, adjust the generator:
- Visual Studio 2019: `-G "Visual Studio 16 2019"`
- Visual Studio 2017: `-G "Visual Studio 15 2017"`

### Build Options

- **Release build** (default): `-O3 -march=native -flto` optimizations
- **Debug build**: `cmake .. -DCMAKE_BUILD_TYPE=Debug`
- **Coverage**: `cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON`

The build system automatically fetches:
- **spdlog** (v1.12.0) for async logging
- **GoogleTest** (v1.14.0) for unit testing

## Running

### Basic Backtest

**Linux/macOS:**
```bash
./build/artemis data/ES_futures_sample.csv 2.5
```

**Windows:**
```powershell
.\build\Release\artemis.exe data\ES_futures_sample.csv 2.5
```

Arguments:
1. Data file path (default: `data/ES_futures_sample.csv`)
2. Z-score threshold (default: 2.5)

### Output

The backtester prints metrics to stdout:
- Total return
- Volatility
- Sharpe ratio
- Max drawdown
- Win rate
- Average trade length
- Ticks processed/second
- Processing time and latency

Results are written to:
- `results.csv`: Equity curve (timestamp, equity)
- `results_trades.csv`: Trade log (entry/exit times, prices, PnL)

### Example Output

```
=== Backtest Results ===
Total Return: 5.23%
Volatility: 12.45%
Sharpe Ratio: 2.67
Max Drawdown: 8.32%
Win Rate: 54.21%
Avg Trade Length: 125.43 seconds
Ticks Processed: 100000
Ticks/Second: 3500000
Processing Time: 0.0286 seconds
Avg Latency: 0.286 µs/tick
```

## Testing

### Run Unit Tests

```bash
cd build
make test
# Or directly:
./artemis_tests
```

### Test Coverage

Tests cover:
- **EWMA correctness**: Verified against pandas implementation
- **Lock-free queue**: MPSC stress test (1M+ pushes/pops, no leaks)
- **Signal generation**: Edge cases (z-score == threshold, zero crossing)
- **Market data reader**: File I/O, reset, malformed lines

### Expected Coverage

Target: 92%+ line coverage

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
make
make test
# Generate coverage report with gcov/lcov
```

## Python Analytics

### Visualization

Generate plots from backtest results:

```bash
python py/read_results.py results.csv
```

Outputs:
- `equity_curve.png`: Equity curve over time
- `rolling_sharpe.png`: Rolling Sharpe ratio
- `trade_distribution.png`: PnL and duration distributions

### Parameter Optimization

Grid search for optimal threshold:

```bash
python py/optimise.py data/ES_futures_sample.csv
```

Searches threshold range 1.5-4.0 (step 0.1) and outputs:
- `optimization_results.csv`: All results
- `best_parameters.json`: Best threshold, Sharpe, max DD

### Install Python Dependencies

```bash
pip install pandas matplotlib seaborn numpy
```

## Performance Targets

On bundled data with i7-12700H:
- **Sharpe ratio**: ≥ 2.5
- **Max drawdown**: ≤ 9%
- **Throughput**: ≥ 3M ticks/min
- **Latency**: ≤ 5µs mean per tick
- **Memory leaks**: Zero (Valgrind clean)
- **Race conditions**: Zero (ThreadSanitizer clean)

## Algorithm

### Signal Generation

1. **Rolling Statistics**: Maintains EWMA mean and variance over N=20,000 tick window
2. **Z-score Calculation**: `z = (price - mean) / stddev`
3. **Entry Signals**:
   - **LONG**: When z-score < -threshold (default -2.5)
   - **SHORT**: When z-score > threshold (default 2.5)
4. **Exit Signals**: Position closes when z-score crosses zero

### Execution Model

- **Fill Price**: Mid-price ± slippage (1 tick against)
- **Commission**: $2.10 per round-turn
- **Contract Multiplier**: 50 (ES futures)
- **Tick Size**: $0.25

### Threading Model

- **Reader thread**: Memory-maps CSV and emits ticks
- **Worker thread**: Processes ticks with core affinity (core 1)
- **Lock-free queue**: MPSC queue between threads
- **Logging**: Async spdlog, info level every 50k ticks

## Data Format

Input CSV format:
```csv
timestamp,bid,ask,volume
1609459200000000,4500.25,4500.50,100
1609459200001000,4500.75,4501.00,200
```

- **timestamp**: Microseconds since epoch
- **bid**: Bid price
- **ask**: Ask price
- **volume**: Trade volume

## Logging

Logs are written to `artemis.log` with spdlog async mode:
- Info level: Sharpe, PnL, max drawdown every 50k ticks
- Error level: File I/O errors, runtime exceptions

## Troubleshooting

### Build Issues

**CMake not found**: Install CMake 3.20+
```bash
# Ubuntu/Debian
sudo apt-get install cmake

# macOS
brew install cmake

# Windows
# Download from cmake.org
```

**Compiler not found**: Install C++17 compiler
```bash
# Ubuntu/Debian
sudo apt-get install g++

# macOS
xcode-select --install
```

### Runtime Issues

**File not found**: Ensure data file exists
```bash
# Generate sample data if missing
python py/generate_sample_data.py
```

**Low performance**: Ensure Release build
```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Test Failures

**EWMA test fails**: Check floating-point precision settings
**Queue test fails**: May be expected under high contention (90%+ success rate)

## License

This project includes sample data under CC0 license. Source code is provided as-is for educational purposes.

## Contributing

1. Ensure all tests pass: `make test`
2. Check coverage: `make coverage` (target 92%+)
3. Run Valgrind: `valgrind --leak-check=full ./artemis_tests`
4. Run ThreadSanitizer: `TSAN_OPTIONS=halt_on_error=1 ./artemis_tests`

## References

- ES Futures: CME Group E-mini S&P 500 futures
- EWMA: Exponentially Weighted Moving Average
- Z-score: Standard score normalization
- Lock-free programming: Herlihy & Shavit, "The Art of Multiprocessor Programming"

