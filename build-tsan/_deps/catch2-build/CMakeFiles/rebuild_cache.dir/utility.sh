set -e

cd "/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/build-tsan/_deps/catch2-build"
/opt/homebrew/bin/cmake --regenerate-during-build -S$(CMAKE_SOURCE_DIR) -B$(CMAKE_BINARY_DIR)
