#pragma once

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace th {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}

inline int& failures() {
  static int f = 0;
  return f;
}

struct Registrar {
  Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); }
};

inline void fail(const char* file, int line, const std::string& what) {
  ++failures();
  std::fprintf(stderr, "  FAIL %s:%d  %s\n", file, line, what.c_str());
}

// Les chaînes attendues contiennent des octets de contrôle : sans échappement,
// un diff de sortie ANSI est illisible et le test ne sert à rien.
inline std::string show(const std::string& v) {
  std::string out = "\"";
  for (unsigned char c : v) {
    if (c == 0x1b) {
      out += "\\e";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c < 0x20 || c == 0x7f) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\x%02x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

template <class T>
std::string show(const T& v) {
  std::ostringstream os;
  os << v;
  return os.str();
}

}  // namespace th

#define TEST(name)                                        \
  static void test_##name();                              \
  static th::Registrar reg_##name(#name, &test_##name);   \
  static void test_##name()

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) th::fail(__FILE__, __LINE__, "CHECK(" #cond ")");     \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    auto&& _a = (a);                                                        \
    auto&& _b = (b);                                                        \
    if (!(_a == _b)) {                                                      \
      th::fail(__FILE__, __LINE__,                                          \
               "CHECK_EQ(" #a ", " #b ")\n       obtenu = " + th::show(_a) + \
                   "\n       attendu = " + th::show(_b));                   \
    }                                                                       \
  } while (0)
