set -e

cd "/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/build-tsan"
/opt/homebrew/bin/ctest -DMODEL=Experimental -DACTIONS=Update -S CMakeFiles/CTestScript.cmake -V
