set -e

cd "/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/build-tsan"
/opt/homebrew/bin/ctest -DMODEL=Nightly -DACTIONS=MemCheck -S CMakeFiles/CTestScript.cmake -V
