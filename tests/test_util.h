#pragma once

#include <functional>
#include <iostream>
#include <string>
#include <vector>

struct TestCase {
  std::string name;
  std::function<void()> body;
};

std::vector<TestCase> &Tests();
int &Failures();

struct TestRegistrar {
  TestRegistrar(const std::string &name, std::function<void()> body) {
    Tests().push_back({name, std::move(body)});
  }
};

#define TEST(name)                                                             \
  static void name();                                                          \
  static TestRegistrar registrar_##name(#name, name);                          \
  static void name()

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      ++Failures();                                                            \
      std::cerr << "    FAIL " << __FILE__ << ":" << __LINE__ << "  " #condition \
                << "\n";                                                       \
    }                                                                          \
  } while (0)

#define CHECK_EQ(actual, expected)                                             \
  do {                                                                         \
    const auto &lhs_ = (actual);                                               \
    const auto &rhs_ = (expected);                                             \
    if (!(lhs_ == rhs_)) {                                                     \
      ++Failures();                                                            \
      std::cerr << "    FAIL " << __FILE__ << ":" << __LINE__ << "  " #actual  \
                << " == " #expected << "\n      got:      " << lhs_            \
                << "\n      expected: " << rhs_ << "\n";                       \
    }                                                                          \
  } while (0)
