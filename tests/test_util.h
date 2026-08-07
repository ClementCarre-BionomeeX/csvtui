#pragma once

#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

// Writes `contents` to a uniquely named file that is removed on destruction.
class TempCSV {
public:
  explicit TempCSV(const std::string &contents) {
    char name[] = "csvtui-test-XXXXXX";
    const int fd = ::mkstemp(name);
    path_ = name;
    if (fd >= 0)
      ::close(fd);
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out << contents;
  }
  ~TempCSV() { ::unlink(path_.c_str()); }

  TempCSV(const TempCSV &) = delete;
  TempCSV &operator=(const TempCSV &) = delete;

  const std::string &path() const { return path_; }

private:
  std::string path_;
};

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
