#include "test_util.h"

#include "csv_scan.h"
#include "csv_sortrun.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

// A file whose sort order is not its file order, with ties, numbers, text and
// empty cells all present — the four cases the comparator distinguishes.
std::string Generate(size_t rows, unsigned seed) {
  std::mt19937 rng(seed);
  std::ostringstream out;
  out << "id,name,score\n";
  static const char *const names[] = {"delta", "alpha", "charlie", "bravo",
                                      "", "Echo", "12", "3.5"};
  for (size_t i = 0; i < rows; ++i) {
    out << i << ',' << names[rng() % 8] << ',' << (rng() % 1000) << '\n';
  }
  return out.str();
}

csvscan::Request RequestFor(const std::string &path, const std::string &csv) {
  csvscan::Request request;
  request.path = path;
  request.delimiter = ',';
  request.chunk_size = 64;
  request.data_offset =
      std::streampos(csv.find('\n') + 1); // past the header
  request.want_order = true;
  request.sort = true;
  return request;
}

// Counts the spill files left in a directory. They are the only things named
// this way, and only RunStore::Spill ever creates one.
size_t RunFilesIn(const std::string &directory) {
  const std::string command =
      "ls " + directory + "/csvtui-sort-* 2>/dev/null | wc -l";
  FILE *pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr)
    return 0;
  char buffer[64] = {0};
  size_t found = 0;
  if (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
    found = static_cast<size_t>(std::strtoul(buffer, nullptr, 10));
  ::pclose(pipe);
  return found;
}

} // namespace

// --- the comparator ----------------------------------------------------------

TEST(SortOrderPutsNumbersBeforeTextBothWays) {
  csvsort::Key number;
  number.numeric = true;
  number.number = 5.0;
  number.row = 1;
  csvsort::Key text;
  text.text = "apple";
  text.row = 0;

  CHECK(csvsort::Order{false}(number, text));
  CHECK(!csvsort::Order{false}(text, number));
  // Descending reverses the values, not the numbers-first rule: empty and
  // non-numeric cells belong at the end either way.
  CHECK(csvsort::Order{true}(number, text));
  CHECK(!csvsort::Order{true}(text, number));
}

TEST(SortOrderBreaksTiesByRowInBothDirections) {
  csvsort::Key first;
  first.text = "same";
  first.row = 3;
  csvsort::Key second;
  second.text = "same";
  second.row = 9;

  CHECK(csvsort::Order{false}(first, second));
  CHECK(!csvsort::Order{false}(second, first));
  // Ties keep file order even when sorting descending, which is what makes a
  // second sort refine the first.
  CHECK(csvsort::Order{true}(first, second));
  CHECK(!csvsort::Order{true}(second, first));
}

// --- spilling ----------------------------------------------------------------

// The property that matters: a sort that spills must give the same answer as
// one that does not. Everything else about the merge is an implementation
// detail, but this cannot be allowed to drift.
TEST(SpilledSortMatchesInMemorySortExactly) {
  for (unsigned seed : {1u, 2u, 3u}) {
    const std::string csv = Generate(4000, seed);
    TempCSV file(csv);

    for (bool descending : {false, true}) {
      for (size_t column : {size_t{1}, size_t{2}}) {
        csvscan::Request request = RequestFor(file.path(), csv);
        request.sort_column = column;
        request.sort_descending = descending;

        csvscan::Result in_memory;
        request.sort_memory_budget = 0;
        CHECK(csvscan::Run(request, in_memory, nullptr, nullptr) ==
              csvscan::Outcome::Done);
        CHECK_EQ(in_memory.spilled_runs, size_t{0});

        // Small enough to force many runs out of 4000 rows.
        csvscan::Result spilled;
        request.sort_memory_budget = 8 * 1024;
        CHECK(csvscan::Run(request, spilled, nullptr, nullptr) ==
              csvscan::Outcome::Done);
        CHECK(spilled.spilled_runs > 1);

        CHECK_EQ(spilled.order.size(), in_memory.order.size());
        CHECK(spilled.order == in_memory.order);
        CHECK_EQ(spilled.total_rows, in_memory.total_rows);
      }
    }
  }
}

TEST(SpilledSortHandlesABudgetSmallerThanOneKey) {
  const std::string csv = Generate(500, 7);
  TempCSV file(csv);

  csvscan::Request request = RequestFor(file.path(), csv);
  request.sort_column = 1;

  csvscan::Result reference;
  CHECK(csvscan::Run(request, reference, nullptr, nullptr) ==
        csvscan::Outcome::Done);

  // One byte: every single key spills as its own run. Absurd, but it must not
  // lose or duplicate a row.
  request.sort_memory_budget = 1;
  csvscan::Result spilled;
  CHECK(csvscan::Run(request, spilled, nullptr, nullptr) ==
        csvscan::Outcome::Done);
  CHECK_EQ(spilled.order.size(), size_t{500});
  CHECK(spilled.order == reference.order);
}

TEST(SpilledSortStillFiltersFirst) {
  const std::string csv = Generate(3000, 11);
  TempCSV file(csv);

  csvscan::Request request = RequestFor(file.path(), csv);
  request.sort_column = 2;
  request.filter = true;
  request.filter_pattern = "alpha";

  csvscan::Result in_memory;
  CHECK(csvscan::Run(request, in_memory, nullptr, nullptr) ==
        csvscan::Outcome::Done);

  request.sort_memory_budget = 4 * 1024;
  csvscan::Result spilled;
  CHECK(csvscan::Run(request, spilled, nullptr, nullptr) ==
        csvscan::Outcome::Done);

  CHECK(spilled.order == in_memory.order);
  CHECK(spilled.order.size() < spilled.total_rows); // the filter did something
  CHECK_EQ(spilled.total_rows, size_t{3000});
}

TEST(SpilledSortKeepsEveryRowExactlyOnce) {
  const std::string csv = Generate(2500, 13);
  TempCSV file(csv);

  csvscan::Request request = RequestFor(file.path(), csv);
  request.sort_column = 1;
  request.sort_memory_budget = 6 * 1024;

  csvscan::Result result;
  CHECK(csvscan::Run(request, result, nullptr, nullptr) ==
        csvscan::Outcome::Done);
  CHECK_EQ(result.order.size(), size_t{2500});

  std::vector<size_t> seen = result.order;
  std::sort(seen.begin(), seen.end());
  for (size_t i = 0; i < seen.size(); ++i)
    CHECK_EQ(seen[i], i);
}

TEST(SpilledSortRemovesItsTemporaryFiles) {
  const std::string directory = csvsort::TempDirectory();
  const size_t before = RunFilesIn(directory);

  {
    const std::string csv = Generate(3000, 17);
    TempCSV file(csv);
    csvscan::Request request = RequestFor(file.path(), csv);
    request.sort_column = 1;
    request.sort_memory_budget = 4 * 1024;

    csvscan::Result result;
    CHECK(csvscan::Run(request, result, nullptr, nullptr) ==
          csvscan::Outcome::Done);
    CHECK(result.spilled_runs > 1);
  }

  CHECK_EQ(RunFilesIn(directory), before);
}

TEST(CancelledSpilledSortLeavesNothingBehind) {
  const std::string directory = csvsort::TempDirectory();
  const size_t before = RunFilesIn(directory);

  const std::string csv = Generate(20000, 19);
  TempCSV file(csv);
  csvscan::Request request = RequestFor(file.path(), csv);
  request.sort_column = 1;
  request.sort_memory_budget = 4 * 1024;

  // Cancel after enough rows that several runs have already been written.
  size_t seen = 0;
  csvscan::Result result;
  const auto outcome = csvscan::Run(
      request, result, [&seen] { return ++seen > 8000; }, nullptr);

  CHECK(outcome == csvscan::Outcome::Cancelled);
  // Gigabytes of abandoned runs in /tmp would be worse than the memory the
  // spilling saved.
  CHECK_EQ(RunFilesIn(directory), before);
}

TEST(SortRunReportsWhereItCannotWrite) {
  csvsort::RunStore store("/definitely/not/a/directory");
  std::vector<csvsort::Key> keys(4);
  for (size_t i = 0; i < keys.size(); ++i)
    keys[i].row = i;

  CHECK(!store.Spill(keys, csvsort::Order{}));
  CHECK(!store.error().empty());
  // The message has to name the directory, since the fix is to point
  // CSVTUI_TMPDIR somewhere with room.
  CHECK(store.error().find("/definitely/not/a/directory") != std::string::npos);
}

TEST(TempDirectoryHonoursTheOverride) {
  const char *previous = ::getenv("CSVTUI_TMPDIR");
  const std::string saved = previous ? previous : "";

  ::setenv("CSVTUI_TMPDIR", "/var/tmp/", 1);
  CHECK_EQ(csvsort::TempDirectory(), std::string("/var/tmp")); // slash trimmed

  ::unsetenv("CSVTUI_TMPDIR");
  CHECK(!csvsort::TempDirectory().empty());

  if (!saved.empty())
    ::setenv("CSVTUI_TMPDIR", saved.c_str(), 1);
}
