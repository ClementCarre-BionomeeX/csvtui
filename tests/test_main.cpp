#include "test_util.h"

std::vector<TestCase> &Tests() {
  static std::vector<TestCase> tests;
  return tests;
}

int &Failures() {
  static int failures = 0;
  return failures;
}

int main() {
  int failed_tests = 0;
  for (const auto &test : Tests()) {
    const int before = Failures();
    test.body();
    const bool ok = Failures() == before;
    if (!ok)
      ++failed_tests;
    std::cout << (ok ? "  ok   " : "  FAIL ") << test.name << "\n";
  }

  std::cout << "\n"
            << Tests().size() << " tests, " << failed_tests << " failed, "
            << Failures() << " check(s) failed\n";
  return Failures() == 0 ? 0 : 1;
}
