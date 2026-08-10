#include <string>

#include "harness.hpp"

TEST(harness_reports_equality) {
  CHECK_EQ(2 + 2, 4);
  CHECK(true);
}

TEST(harness_escapes_control_bytes) {
  CHECK_EQ(th::show(std::string("\033[0m")), std::string("\"\\e[0m\""));
}
