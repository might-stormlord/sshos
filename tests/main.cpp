#include <cstring>

#include "harness.hpp"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  int failed_cases = 0;

  for (const auto& c : th::registry()) {
    if (filter != nullptr && std::strstr(c.name, filter) == nullptr) continue;
    const int before = th::failures();
    std::printf("- %s\n", c.name);
    c.fn();
    ++ran;
    if (th::failures() > before) ++failed_cases;
  }

  std::printf("\n%d cas, %d en echec, %d assertions echouees\n", ran,
              failed_cases, th::failures());
  return th::failures() == 0 ? 0 : 1;
}
