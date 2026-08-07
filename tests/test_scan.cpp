#include "test_util.h"

#include "csv_parser.h"
#include "csv_scan.h"

#include <atomic>
#include <sstream>
#include <string>
#include <vector>

namespace {

// A file of `rows` records whose second column cycles through a few names, so
// sorting has ties to keep stable and filtering has a predictable hit rate.
std::string Generate(size_t rows) {
  static const char *const names[] = {"delta", "alpha", "charlie", "bravo"};
  std::ostringstream out;
  out << "id,name,score\n";
  for (size_t i = 0; i < rows; ++i)
    out << i << ',' << names[i % 4] << ',' << (rows - i) << '\n';
  return out.str();
}

csvscan::Request RequestFor(const std::string &path) {
  csvscan::Request request;
  request.path = path;
  request.delimiter = ',';
  request.chunk_size = 8;
  // Past the header, which the model would normally have consumed.
  request.data_offset = std::streampos(std::string("id,name,score\n").size());
  return request;
}

std::vector<std::string> ColumnInOrder(const std::string &csv,
                                       const csvscan::Result &result,
                                       size_t col) {
  // Re-read the file the naive way so the scan is checked against something
  // that shares none of its code.
  std::vector<std::string> lines;
  std::istringstream in(csv);
  std::string line;
  std::getline(in, line); // header
  while (std::getline(in, line))
    lines.push_back(line);

  std::vector<std::string> values;
  for (size_t row : result.order) {
    const auto fields = csv::SplitRecord(lines[row], ',');
    values.push_back(col < fields.size() ? fields[col] : std::string());
  }
  return values;
}

} // namespace

// --- the single-column extractor --------------------------------------------

TEST(ExtractFieldMatchesSplitRecord) {
  const std::string records[] = {
      "a,b,c",
      "",
      ",,",
      "\"Dupont, Jean\",x,y",
      "\"he said \"\"hi\"\"\",tail",
      "trailing,",
      "\"multi\nline\",z",
  };

  std::string got, scratch;
  for (const std::string &record : records) {
    const auto fields = csv::SplitRecord(record, ',');
    // One past the end too: asking for a field that is not there must give an
    // empty string rather than the last one.
    for (size_t i = 0; i <= fields.size(); ++i) {
      csv::ExtractField(record, ',', i, got, scratch);
      const std::string expected = i < fields.size() ? fields[i] : std::string();
      CHECK_EQ(got, expected);
    }
  }
}

TEST(ExtractFieldDoesNotLeaveTheLastValueBehind) {
  // Both buffers are handed back in, so a long value followed by a short one
  // must not leave the tail of the long one in either.
  std::string got, scratch;
  csv::ExtractField("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa,x", ',', 0, got, scratch);
  CHECK_EQ(got, std::string("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
  csv::ExtractField("b,x", ',', 0, got, scratch);
  CHECK_EQ(got, std::string("b"));
}

TEST(ExtractedFieldDoesNotCarryTheBufferCapacity) {
  // The reason `out` and `scratch` are separate strings: walking past a long
  // field must not leave every extracted key holding a buffer sized for it.
  // A million sort keys each carrying a spare heap block is the difference
  // between a sort fitting in memory and not.
  std::string got, scratch;
  csv::ExtractField("id,a-very-long-email-address@example.com,7", ',', 2, got,
                    scratch);
  CHECK_EQ(got, std::string("7"));
  CHECK(got.capacity() < 32); // still the small-string buffer
  CHECK(scratch.capacity() >= 32);
}

TEST(RecordContainsMatchesFieldWiseSearch) {
  std::string scratch;
  // The delimiter inside a quoted field is part of the value, so a pattern
  // spanning it must match; one spanning a real delimiter must not.
  CHECK(csv::RecordContains("\"Dupont, Jean\",x", ',', "Dupont, Jean", false,
                            scratch));
  CHECK(!csv::RecordContains("Dupont,Jean", ',', "Dupont,Jean", false, scratch));
  // Quotes are syntax, not content.
  CHECK(!csv::RecordContains("\"quoted\",x", ',', "\"quoted\"", false, scratch));
  CHECK(csv::RecordContains("\"he said \"\"hi\"\"\"", ',', "said \"hi\"", false,
                            scratch));
  // Case folding follows the flag.
  CHECK(csv::RecordContains("Alpha,b", ',', "alpha", true, scratch));
  CHECK(!csv::RecordContains("Alpha,b", ',', "alpha", false, scratch));
}

// --- the pass ----------------------------------------------------------------

TEST(ScanCountsRowsAndBuildsOffsets) {
  const std::string csv = Generate(100);
  TempCSV file(csv);

  csvscan::Result result;
  const auto outcome =
      csvscan::Run(RequestFor(file.path()), result, nullptr, nullptr);

  CHECK(outcome == csvscan::Outcome::Done);
  CHECK_EQ(result.total_rows, size_t{100});
  // One offset for the start plus one per completed chunk of 8.
  CHECK_EQ(result.offsets.size(), size_t{1 + 100 / 8});
  // A pass that was not asked for an ordering must not build one.
  CHECK(!result.has_order);
  CHECK(result.order.empty());
}

TEST(ScanSortsAscendingAndDescending) {
  const std::string csv = Generate(200);
  TempCSV file(csv);

  csvscan::Request request = RequestFor(file.path());
  request.sort = true;
  request.sort_column = 1; // name
  request.want_order = true;

  csvscan::Result ascending;
  CHECK(csvscan::Run(request, ascending, nullptr, nullptr) ==
        csvscan::Outcome::Done);
  CHECK(ascending.has_order);
  CHECK_EQ(ascending.order.size(), size_t{200});

  const auto names = ColumnInOrder(csv, ascending, 1);
  CHECK_EQ(names.front(), std::string("alpha"));
  CHECK_EQ(names.back(), std::string("delta"));
  for (size_t i = 1; i < names.size(); ++i)
    CHECK(names[i - 1] <= names[i]);

  request.sort_descending = true;
  csvscan::Result descending;
  CHECK(csvscan::Run(request, descending, nullptr, nullptr) ==
        csvscan::Outcome::Done);
  const auto reversed = ColumnInOrder(csv, descending, 1);
  CHECK_EQ(reversed.front(), std::string("delta"));
  CHECK_EQ(reversed.back(), std::string("alpha"));
}

TEST(ScanSortsNumericColumnsNumerically) {
  // 10 must not land between 1 and 2 the way a string sort would put it.
  TempCSV file("id,score\na,2\nb,10\nc,1\n");
  csvscan::Request request;
  request.path = file.path();
  request.data_offset = std::streampos(std::string("id,score\n").size());
  request.sort = true;
  request.sort_column = 1;
  request.want_order = true;

  csvscan::Result result;
  CHECK(csvscan::Run(request, result, nullptr, nullptr) ==
        csvscan::Outcome::Done);
  CHECK_EQ(result.order.size(), size_t{3});
  CHECK_EQ(result.order[0], size_t{2}); // 1
  CHECK_EQ(result.order[1], size_t{0}); // 2
  CHECK_EQ(result.order[2], size_t{1}); // 10
}

TEST(ScanSortIsStableAcrossTies) {
  const std::string csv = Generate(40);
  TempCSV file(csv);

  csvscan::Request request = RequestFor(file.path());
  request.sort = true;
  request.sort_column = 1; // ten rows share each name
  request.want_order = true;

  csvscan::Result result;
  CHECK(csvscan::Run(request, result, nullptr, nullptr) ==
        csvscan::Outcome::Done);

  // Within one name, rows must stay in file order.
  const auto names = ColumnInOrder(csv, result, 1);
  for (size_t i = 1; i < result.order.size(); ++i) {
    if (names[i] == names[i - 1])
      CHECK(result.order[i - 1] < result.order[i]);
  }
}

TEST(ScanFiltersToMatchingRows) {
  const std::string csv = Generate(100);
  TempCSV file(csv);

  csvscan::Request request = RequestFor(file.path());
  request.filter = true;
  request.filter_pattern = "alpha";
  request.want_order = true;

  csvscan::Result result;
  CHECK(csvscan::Run(request, result, nullptr, nullptr) ==
        csvscan::Outcome::Done);
  CHECK(result.has_order);
  CHECK_EQ(result.order.size(), size_t{25}); // one row in four
  // Counting is unaffected by the filter: it still sees the whole file.
  CHECK_EQ(result.total_rows, size_t{100});
  for (size_t row : result.order)
    CHECK_EQ(row % 4, size_t{1});
}

TEST(ScanSortsWithinAFilterInOnePass) {
  const std::string csv = Generate(100);
  TempCSV file(csv);

  csvscan::Request request = RequestFor(file.path());
  request.filter = true;
  request.filter_pattern = "alpha";
  request.sort = true;
  request.sort_column = 2; // score, which descends as id ascends
  request.want_order = true;

  csvscan::Result result;
  CHECK(csvscan::Run(request, result, nullptr, nullptr) ==
        csvscan::Outcome::Done);
  CHECK_EQ(result.order.size(), size_t{25});
  // Only filtered rows survive, and they come back in score order — which is
  // the reverse of file order for this generator.
  for (size_t row : result.order)
    CHECK_EQ(row % 4, size_t{1});
  for (size_t i = 1; i < result.order.size(); ++i)
    CHECK(result.order[i - 1] > result.order[i]);
}

TEST(ScanComputesColumnStatsHonouringTheFilter) {
  const std::string csv = Generate(100); // score runs 100 down to 1
  TempCSV file(csv);

  csvscan::Request request = RequestFor(file.path());
  request.want_stats = true;
  request.stats_column = 2;

  csvscan::Result all;
  CHECK(csvscan::Run(request, all, nullptr, nullptr) == csvscan::Outcome::Done);
  CHECK_EQ(all.stats.total, size_t{100});
  CHECK_EQ(all.stats.numeric, size_t{100});
  CHECK_EQ(all.stats.empty, size_t{0});
  CHECK_EQ(all.stats.min, 1.0);
  CHECK_EQ(all.stats.max, 100.0);
  CHECK_EQ(all.stats.mean, 50.5);

  request.filter = true;
  request.filter_pattern = "alpha"; // rows 1, 5, 9, … scoring 99, 95, 91, …
  csvscan::Result filtered;
  CHECK(csvscan::Run(request, filtered, nullptr, nullptr) ==
        csvscan::Outcome::Done);
  CHECK_EQ(filtered.stats.total, size_t{25});
  CHECK_EQ(filtered.stats.min, 3.0);
  CHECK_EQ(filtered.stats.max, 99.0);
}

TEST(ScanCountsEmptyCellsSeparatelyFromNonNumericOnes) {
  TempCSV file("id,value\na,\nb,x\nc,3\n");
  csvscan::Request request;
  request.path = file.path();
  request.data_offset = std::streampos(std::string("id,value\n").size());
  request.want_stats = true;
  request.stats_column = 1;

  csvscan::Result result;
  CHECK(csvscan::Run(request, result, nullptr, nullptr) ==
        csvscan::Outcome::Done);
  CHECK_EQ(result.stats.total, size_t{3});
  CHECK_EQ(result.stats.empty, size_t{1});
  CHECK_EQ(result.stats.numeric, size_t{1});
}

TEST(ScanStopsWhenCancelled) {
  const std::string csv = Generate(5000);
  TempCSV file(csv);

  csvscan::Request request = RequestFor(file.path());
  request.want_order = true;
  request.sort = true;

  csvscan::Result result;
  const auto outcome = csvscan::Run(
      request, result, [] { return true; }, nullptr);
  CHECK(outcome == csvscan::Outcome::Cancelled);
}

TEST(ScanReportsProgressWhileRunning) {
  const std::string csv = Generate(20);
  TempCSV file(csv);

  size_t reports = 0;
  double last_fraction = -1.0;
  size_t last_rows = 0;
  csvscan::Result result;
  csvscan::Run(RequestFor(file.path()), result, nullptr,
               [&](const csvscan::Progress &progress) {
                 ++reports;
                 last_fraction = progress.fraction;
                 last_rows = progress.rows;
               });
  // Small files report only the final tick, which must say "finished" and
  // agree with the result it accompanies.
  CHECK(reports >= 1);
  CHECK_EQ(last_fraction, 1.0);
  CHECK_EQ(last_rows, result.total_rows);
  CHECK_EQ(last_rows, size_t{20});
}

TEST(ScanFailsCleanlyOnAMissingFile) {
  csvscan::Request request;
  request.path = "csvtui-no-such-file-here.csv";
  csvscan::Result result;
  CHECK(csvscan::Run(request, result, nullptr, nullptr) ==
        csvscan::Outcome::Failed);
}

TEST(ScanHandlesRecordsSpanningTwoLines) {
  // The row numbers it reports are records, not lines — the thing `wc -l` gets
  // wrong on a file like this.
  TempCSV file("id,name\n1,\"Marie\nCurie\"\n2,Ada\n");
  csvscan::Request request;
  request.path = file.path();
  request.data_offset = std::streampos(std::string("id,name\n").size());
  request.filter = true;
  request.filter_pattern = "Curie";
  request.want_order = true;

  csvscan::Result result;
  CHECK(csvscan::Run(request, result, nullptr, nullptr) ==
        csvscan::Outcome::Done);
  CHECK_EQ(result.total_rows, size_t{2});
  CHECK_EQ(result.order.size(), size_t{1});
  CHECK_EQ(result.order[0], size_t{0});
}

// --- the worker --------------------------------------------------------------

TEST(ScannerRunsOnAThreadAndHandsBackTheSameResult) {
  const std::string csv = Generate(2000);
  TempCSV file(csv);

  csvscan::Request request = RequestFor(file.path());
  request.sort = true;
  request.sort_column = 1;
  request.want_order = true;

  csvscan::Result expected;
  CHECK(csvscan::Run(request, expected, nullptr, nullptr) ==
        csvscan::Outcome::Done);

  std::atomic<int> notifications{0};
  CSVScanner scanner;
  scanner.Start(request, [&] { ++notifications; });
  scanner.Join();

  CHECK(scanner.state() == CSVScanner::State::Done);
  CSVScanner::Result actual;
  CHECK(scanner.Take(actual));
  CHECK_EQ(actual.total_rows, expected.total_rows);
  CHECK(actual.order == expected.order);
  CHECK(notifications.load() >= 1);

  // Take() leaves it reusable.
  CHECK(scanner.state() == CSVScanner::State::Idle);
  CHECK(!scanner.Take(actual));
}

TEST(ScannerCancelsPromptly) {
  const std::string csv = Generate(200000);
  TempCSV file(csv);

  csvscan::Request request = RequestFor(file.path());
  request.want_order = true;
  request.sort = true;

  CSVScanner scanner;
  scanner.Start(request, nullptr);
  scanner.Cancel();
  scanner.Join();

  CHECK(scanner.state() == CSVScanner::State::Cancelled);
  // A cancelled pass yields nothing, so a caller cannot mistake a partial
  // ordering for a complete one.
  CSVScanner::Result result;
  CHECK(!scanner.Take(result));
}

TEST(ScannerReportsFailureWithoutBlocking) {
  csvscan::Request request;
  request.path = "csvtui-no-such-file-here.csv";

  CSVScanner scanner;
  scanner.Start(request, nullptr);
  scanner.Join();
  CHECK(scanner.state() == CSVScanner::State::Failed);
}
