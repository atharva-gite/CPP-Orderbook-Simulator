# CMake generated Testfile for 
# Source directory: /Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator
# Build directory: /Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/build-tsan
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test("lob_tests" "/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/build-tsan/lob_tests")
set_tests_properties("lob_tests" PROPERTIES  _BACKTRACE_TRIPLES "/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/CMakeLists.txt;116;add_test;/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/CMakeLists.txt;0;")
add_test("lob_order_tests" "/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/build-tsan/lob_order_tests")
set_tests_properties("lob_order_tests" PROPERTIES  _BACKTRACE_TRIPLES "/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/CMakeLists.txt;121;add_test;/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/CMakeLists.txt;0;")
add_test("lob_matching_tests" "/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/build-tsan/lob_matching_tests")
set_tests_properties("lob_matching_tests" PROPERTIES  _BACKTRACE_TRIPLES "/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/CMakeLists.txt;127;add_test;/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/CMakeLists.txt;0;")
add_test("lob_concurrent_tests" "/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/build-tsan/lob_concurrent_tests")
set_tests_properties("lob_concurrent_tests" PROPERTIES  _BACKTRACE_TRIPLES "/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/CMakeLists.txt;132;add_test;/Users/atharvagite/Desktop/QuantFin/Cpp OrderBook Simulator/CMakeLists.txt;0;")
subdirs("_deps/catch2-build")
