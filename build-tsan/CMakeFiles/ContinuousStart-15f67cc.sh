set -e

cd "/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/build-tsan"
/opt/homebrew/bin/ctest -DMODEL=Continuous -DACTIONS=Start -S CMakeFiles/CTestScript.cmake -V
