// libFuzzer entry point for the parsing and scanning code.
//
// Build it with:
//
//   cmake -S . -B build-fuzz -DCSVTUI_BUILD_FUZZERS=ON
//         -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
//         -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
//   cmake --build build-fuzz -j
//   ./build-fuzz/fuzz_parser -max_total_time=60 tests/fuzz/corpus
//
// The input is treated as a whole CSV file, so the fuzzer gets to choose the
// delimiter, the quoting, the line endings and the byte sequences inside the
// fields. Undefined behaviour on any of those is what took the viewer down in
// the field: a UTF-8 lead byte is negative as a plain `char`, and handing one
// to a <cctype> function is undefined and does crash on some libcs.

#include "csv_parser.h"
#include "csv_scan.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

// Writes the input to a real file, because the scanner reads files rather than
// buffers. Removed before returning.
class ScopedFile {
public:
  explicit ScopedFile(const uint8_t *data, size_t size) {
    char name[] = "/tmp/csvtui-fuzz-XXXXXX";
    fd_ = ::mkstemp(name);
    path_ = name;
    if (fd_ >= 0) {
      // A short write just means the fuzzer sees a truncated file, which is
      // itself a case worth exercising.
      const ssize_t written = ::write(fd_, data, size);
      (void)written;
      ::close(fd_);
    }
  }
  ~ScopedFile() {
    if (fd_ >= 0)
      ::unlink(path_.c_str());
  }
  const std::string &path() const { return path_; }
  bool ok() const { return fd_ >= 0; }

private:
  int fd_ = -1;
  std::string path_;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size == 0 || size > (1u << 20))
    return 0;

  const std::string text(reinterpret_cast<const char *>(data), size);

  // The pure string helpers, on the raw input and on each of its lines.
  const char delimiter = csv::DetectDelimiter(text);
  const std::vector<std::string> fields = csv::SplitRecord(text, delimiter);
  csv::DisplayWidth(text);
  csv::SanitizeForDisplay(text);
  csv::TruncateToWidth(text, static_cast<int>(size % 40));
  csv::IsNumeric(text);
  double number = 0.0;
  csv::ParseNumber(text, number);
  csv::SmartCaseInsensitive(text);

  // Walking backwards over UTF-8 must terminate and stay in bounds whatever
  // the byte soup, since it is what Backspace does at a prompt.
  for (size_t i = text.size(); i > 0;)
    i = csv::PrevCharBoundary(text, i);

  // The single-column path must agree with splitting everything, for every
  // index including one past the end.
  std::string got, scratch;
  for (size_t i = 0; i <= fields.size(); ++i) {
    csv::ExtractField(text, delimiter, i, got, scratch);
    const std::string expected = i < fields.size() ? fields[i] : std::string();
    if (got != expected) {
      std::fprintf(stderr, "ExtractField disagrees with SplitRecord at %zu\n", i);
      __builtin_trap();
    }
  }

  // And searching a record must agree with searching its fields one by one.
  if (!fields.empty()) {
    const std::string &needle = fields.front();
    if (!needle.empty() && needle.size() < 64) {
      const bool ignore_case = csv::SmartCaseInsensitive(needle);
      bool expected = false;
      for (const std::string &field : fields) {
        if (csv::FindFrom(field, needle, 0, ignore_case) != std::string::npos) {
          expected = true;
          break;
        }
      }
      if (csv::RecordContains(text, delimiter, needle, ignore_case, scratch) !=
          expected) {
        std::fprintf(stderr, "RecordContains disagrees with a field-wise scan\n");
        __builtin_trap();
      }
    }
  }

  // Finally the whole streaming pass, which is what a sort or filter runs.
  ScopedFile file(data, size);
  if (file.ok()) {
    csvscan::Request request;
    request.path = file.path();
    request.delimiter = delimiter;
    request.chunk_size = 4; // exercise the offset table on tiny inputs
    request.want_order = true;
    request.sort = true;
    request.sort_column = size % 8;
    request.filter = (size % 3) == 0;
    request.filter_pattern = fields.empty() ? std::string() : fields.front();
    request.want_stats = true;
    request.stats_column = size % 8;

    csvscan::Result result;
    csvscan::Run(request, result, nullptr, nullptr);

    // Every row in the ordering must be a row that exists.
    for (size_t row : result.order) {
      if (row >= result.total_rows) {
        std::fprintf(stderr, "ordering names row %zu of %zu\n", row,
                     result.total_rows);
        __builtin_trap();
      }
    }
  }

  return 0;
}
