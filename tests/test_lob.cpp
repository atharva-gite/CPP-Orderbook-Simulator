#include <catch2/catch_test_macros.hpp>
#include "lob/lob.h"

TEST_CASE("add works", "[lob]") {
    REQUIRE(lob::add(1, 2) == 3);
}
