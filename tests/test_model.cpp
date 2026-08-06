#include "test_util.h"

#include "csv_model.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

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

  const std::string &path() const { return path_; }

private:
  std::string path_;
};

std::string Cell(CSVModel &model, size_t row, size_t col) {
  std::vector<std::string> fields;
  if (!model.GetRow(row, fields))
    return "<no row>";
  return col < fields.size() ? fields[col] : "<no col>";
}

} // namespace

TEST(OpenReportsMissingFile) {
  CSVModel model;
  const std::string error = model.Open("/definitely/not/here.csv", {}, {});
  CHECK(!error.empty());
  CHECK(error.find("no such file") != std::string::npos);
}

// Regression: a directory and an empty file both used to report
// "Failed to open file", which was misleading in both cases.
TEST(OpenReportsDirectory) {
  CSVModel model;
  const std::string error = model.Open("/tmp", {}, {});
  CHECK(error.find("is a directory") != std::string::npos);
}

TEST(OpenReportsEmptyFile) {
  TempCSV file("");
  CSVModel model;
  const std::string error = model.Open(file.path(), {}, {});
  CHECK(error.find("is empty") != std::string::npos);
}

TEST(ReadsHeaderAndRows) {
  TempCSV file("id,name,score\n1,alpha,10\n2,beta,20\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  CHECK_EQ(model.Header().size(), size_t{3});
  CHECK_EQ(model.Header()[1], std::string("name"));
  CHECK_EQ(model.RowCount(), size_t{2});
  CHECK_EQ(Cell(model, 0, 1), std::string("alpha"));
  CHECK_EQ(Cell(model, 1, 2), std::string("20"));
  CHECK_EQ(model.delimiter(), ',');
}

// Regression for the audit's worst data bug: quoted commas silently shifted
// every column to their right.
TEST(QuotedFieldsDoNotShiftColumns) {
  TempCSV file("id,nom,ville,note\n1,\"Dupont, Jean\",Geneve,12\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  CHECK_EQ(Cell(model, 0, 1), std::string("Dupont, Jean"));
  CHECK_EQ(Cell(model, 0, 2), std::string("Geneve"));
  CHECK_EQ(Cell(model, 0, 3), std::string("12"));
}

TEST(MultilineQuotedFieldIsOneRow) {
  TempCSV file("id,note\n1,\"first\nsecond\"\n2,plain\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  CHECK_EQ(model.RowCount(), size_t{2});
  CHECK_EQ(Cell(model, 0, 1), std::string("first\nsecond"));
  CHECK_EQ(Cell(model, 1, 1), std::string("plain"));
}

TEST(CrlfFileParsesCleanly) {
  TempCSV file("id,name\r\n1,alpha\r\n2,beta\r\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  CHECK_EQ(model.RowCount(), size_t{2});
  CHECK_EQ(Cell(model, 1, 1), std::string("beta"));
  CHECK_EQ(model.Header()[1], std::string("name"));
}

TEST(BomIsStripped) {
  TempCSV file("\xEF\xBB\xBFid,name\n1,alpha\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  CHECK_EQ(model.Header()[0], std::string("id"));
}

// Regression: has_header was hardcoded, so a headerless file lost its first row.
TEST(NoHeaderModeKeepsFirstRow) {
  TempCSV file("1,alpha\n2,beta\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, false), std::string(""));
  CHECK_EQ(model.RowCount(), size_t{2});
  CHECK_EQ(Cell(model, 0, 1), std::string("alpha"));
  CHECK_EQ(model.Header()[0], std::string("col1"));
}

TEST(DelimiterOverrideIsHonoured) {
  TempCSV file("a|b\n1|2\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), '|', {}), std::string(""));
  CHECK_EQ(model.Header().size(), size_t{2});
  CHECK_EQ(Cell(model, 0, 1), std::string("2"));
}

TEST(RowsBeyondEndAreRefused) {
  TempCSV file("a,b\n1,2\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  std::vector<std::string> fields;
  CHECK(model.GetRow(0, fields));
  CHECK(!model.GetRow(1, fields));
  CHECK(!model.GetRow(999999, fields));
}

TEST(HeaderOnlyFileHasNoRows) {
  TempCSV file("a,b,c\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  CHECK_EQ(model.RowCount(), size_t{0});
  std::vector<std::string> fields;
  CHECK(!model.GetRow(0, fields));
}

TEST(FileLargerThanOneChunk) {
  std::string contents = "id,name\n";
  for (int i = 0; i < 2500; ++i)
    contents += std::to_string(i) + ",name" + std::to_string(i) + "\n";
  TempCSV file(contents);

  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  CHECK_EQ(model.RowCount(), size_t{2500});
  CHECK_EQ(Cell(model, 0, 1), std::string("name0"));
  CHECK_EQ(Cell(model, 1200, 1), std::string("name1200"));
  CHECK_EQ(Cell(model, 2499, 1), std::string("name2499"));
  // Re-reading an evicted chunk must still work.
  CHECK_EQ(Cell(model, 5, 1), std::string("name5"));
}

TEST(SearchFindsForwardAndBackward) {
  TempCSV file("id,name\n1,alpha\n2,beta\n3,gamma\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));

  auto hit = model.FindNext("beta", 0, 0, false);
  CHECK(hit.has_value());
  CHECK_EQ(hit->row, size_t{1});
  CHECK_EQ(hit->col, size_t{1});

  auto back = model.FindPrev("alpha", 2, 0, false);
  CHECK(back.has_value());
  CHECK_EQ(back->row, size_t{0});

  CHECK(!model.FindNext("nowhere", 0, 0, true).has_value());
}

TEST(SearchIsSmartCase) {
  TempCSV file("id,name\n1,Alpha\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  CHECK(model.FindNext("alpha", 0, 0, false).has_value());  // lowercase: fuzzy
  CHECK(!model.FindNext("ALPHA", 0, 0, false).has_value()); // uppercase: exact
}

TEST(SearchWraps) {
  TempCSV file("id,name\n1,alpha\n2,beta\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  auto hit = model.FindNext("alpha", 1, 0, true);
  CHECK(hit.has_value());
  CHECK_EQ(hit->row, size_t{0});
  CHECK(!model.FindNext("alpha", 1, 0, false).has_value());
}

// Regression: `n` could not reach a second match in the same row because the
// search restarted from the viewport row rather than the cursor cell.
TEST(SearchFindsSecondMatchInSameRow) {
  TempCSV file("a,b,c\nx,x,x\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  auto first = model.FindNext("x", 0, 0, false);
  CHECK(first.has_value());
  CHECK_EQ(first->col, size_t{1});
  auto second = model.FindNext("x", first->row, first->col, false);
  CHECK(second.has_value());
  CHECK_EQ(second->col, size_t{2});
}

TEST(SortByNumericColumn) {
  TempCSV file("id,score\na,30\nb,4\nc,100\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));

  model.SortByColumn(1, false);
  CHECK_EQ(Cell(model, 0, 1), std::string("4"));
  CHECK_EQ(Cell(model, 2, 1), std::string("100")); // numeric, not lexical

  model.SortByColumn(1, true);
  CHECK_EQ(Cell(model, 0, 1), std::string("100"));

  model.ClearSort();
  CHECK_EQ(Cell(model, 0, 1), std::string("30"));
}

TEST(SortByTextColumn) {
  TempCSV file("id,name\n1,charlie\n2,alpha\n3,bravo\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  model.SortByColumn(1, false);
  CHECK_EQ(Cell(model, 0, 1), std::string("alpha"));
  CHECK_EQ(Cell(model, 2, 1), std::string("charlie"));
}

TEST(FilterSelectsMatchingRows) {
  TempCSV file("id,name\n1,alpha\n2,beta\n3,alphabet\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));

  CHECK_EQ(model.ApplyFilter("alpha"), size_t{2});
  CHECK_EQ(model.RowCount(), size_t{2});
  CHECK_EQ(Cell(model, 0, 1), std::string("alpha"));
  CHECK_EQ(Cell(model, 1, 1), std::string("alphabet"));

  model.ClearFilter();
  CHECK_EQ(model.RowCount(), size_t{3});
}

TEST(FilterAndSortCombine) {
  TempCSV file("id,name,score\n1,alpha,30\n2,beta,5\n3,alphabet,10\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  model.ApplyFilter("alpha");
  model.SortByColumn(2, false);
  CHECK_EQ(model.RowCount(), size_t{2});
  CHECK_EQ(Cell(model, 0, 2), std::string("10"));
  CHECK_EQ(Cell(model, 1, 2), std::string("30"));
}

TEST(ColumnStatsSummariseNumbers) {
  TempCSV file("id,score\n1,10\n2,20\n3,\n4,oops\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  const auto stats = model.ComputeColumnStats(1);
  CHECK_EQ(stats.total, size_t{4});
  CHECK_EQ(stats.empty, size_t{1});
  CHECK_EQ(stats.numeric, size_t{2});
  CHECK_EQ(stats.min, 10.0);
  CHECK_EQ(stats.max, 20.0);
  CHECK_EQ(stats.mean, 15.0);
}

TEST(NumericColumnDetection) {
  TempCSV file("id,name,score\n1,alpha,10\n2,beta,20\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  CHECK(model.ColumnIsNumeric(0));
  CHECK(!model.ColumnIsNumeric(1));
  CHECK(model.ColumnIsNumeric(2));
}

TEST(RaggedRowsAreTolerated) {
  TempCSV file("a,b\n1,2,3,4\n5\n");
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  CHECK_EQ(model.RowCount(), size_t{2});
  CHECK_EQ(Cell(model, 0, 3), std::string("4"));
  CHECK_EQ(Cell(model, 1, 0), std::string("5"));
  CHECK_EQ(Cell(model, 1, 1), std::string("<no col>"));
  CHECK(model.ColumnCount() >= size_t{4});
}
