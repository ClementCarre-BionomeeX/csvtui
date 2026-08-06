#include "test_util.h"

#include "csv_index.h"
#include "csv_model.h"
#include "csv_system.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

class TempCSV {
public:
  explicit TempCSV(const std::string &contents) {
    char name[] = "csvtui-limit-XXXXXX";
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

std::string ManyRows(int rows) {
  std::string out = "id,name,score\n";
  out.reserve(rows * 20 + 16);
  for (int i = 0; i < rows; ++i)
    out += std::to_string(i) + ",name" + std::to_string(i) + "," +
           std::to_string(i % 100) + "\n";
  return out;
}

} // namespace

TEST(HumanBytesReadsNaturally) {
  CHECK_EQ(csv::HumanBytes(512), std::string("512 B"));
  CHECK_EQ(csv::HumanBytes(2048), std::string("2.0 kB"));
  CHECK_EQ(csv::HumanBytes(size_t(10) << 30), std::string("10 GB"));
}

TEST(AvailableMemoryIsPlausible) {
  const size_t available = csv::AvailableMemoryBytes();
  // 0 means "unknown", which is allowed; anything else must be sane.
  if (available != 0)
    CHECK(available > (size_t(1) << 20));
}

TEST(RowCountEstimateIsCloseWithoutScanning) {
  TempCSV file(ManyRows(5000));
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));

  const size_t estimate = model.EstimatedRowCount();
  CHECK(!model.RowCountIsExact()); // nothing has counted yet
  // It is an estimate from a sampled prefix, so it is allowed to be off; it
  // only has to be the right order of magnitude for a guard and a status bar.
  // This fixture is deliberately unkind: ids grow from 1 to 4 digits, so the
  // sampled head is shorter than the tail.
  CHECK(estimate > 4000);
  CHECK(estimate < 6000);

  CHECK_EQ(model.RowCount(), size_t{5000});
  CHECK(model.RowCountIsExact());
  CHECK_EQ(model.EstimatedRowCount(), size_t{5000}); // exact once known
}

// Phase 0: sorting a huge file must refuse rather than exhaust memory.
TEST(SortIsRefusedWhenItWouldNotFit) {
  TempCSV file(ManyRows(5000));
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));

  const size_t rows = model.EstimatedRowCount();
  const size_t needed = rows * CSVModel::kSortBytesPerRow;
  CHECK_EQ(model.EstimatedSortBytes(), needed);

  // Plenty of room: allowed.
  CHECK_EQ(model.CheckSortFeasible(needed * 10), std::string(""));
  // Not enough room: refused, and the message must carry the numbers.
  const std::string refusal = model.CheckSortFeasible(needed / 4);
  CHECK(!refusal.empty());
  CHECK(refusal.find("sorting") != std::string::npos);
  CHECK(refusal.find("filter") != std::string::npos);
  // Unknown memory must never block the user.
  CHECK_EQ(model.CheckSortFeasible(0), std::string(""));
}

TEST(FilterIsCheaperThanSort) {
  TempCSV file(ManyRows(5000));
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  CHECK(model.EstimatedFilterBytes() < model.EstimatedSortBytes());

  const size_t budget = model.EstimatedSortBytes();
  // A budget that blocks sorting can still allow filtering.
  CHECK(!model.CheckSortFeasible(budget).empty());
  CHECK_EQ(model.CheckFilterFeasible(budget), std::string(""));
}

TEST(PositionFractionNeedsNoRowCount) {
  TempCSV file(ManyRows(5000));
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));

  CHECK(!model.RowCountIsExact());
  const double top = model.PositionFraction(0);
  const double middle = model.PositionFraction(2500);
  CHECK(top >= 0.0 && top < 0.1);
  CHECK(middle > top);
  CHECK(middle <= 1.0);
  CHECK(!model.RowCountIsExact()); // asking must not have triggered a scan
}

// Phase 1: the background scan produces the same answer as counting inline.
TEST(IndexerCountsRowsAndOffsets) {
  TempCSV file(ManyRows(3000));
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));

  CSVIndexer indexer;
  indexer.Start(file.path(), model.DataOffset(), CSVModel::kChunkSize,
                model.FileSize(), nullptr);
  indexer.Join();
  CHECK(indexer.state() == CSVIndexer::State::Done);

  CSVIndexer::Result result;
  CHECK(indexer.Take(result));
  CHECK_EQ(result.total_rows, size_t{3000});
  // One offset for the start plus one per completed chunk.
  CHECK_EQ(result.offsets.size(), size_t{3000 / CSVModel::kChunkSize + 1});

  model.AdoptIndex(std::move(result.offsets), result.total_rows);
  CHECK(model.RowCountIsExact());
  CHECK_EQ(model.RowCount(), size_t{3000});

  // Rows must still read correctly through the adopted offsets.
  std::vector<std::string> fields;
  CHECK(model.GetRow(2999, fields));
  CHECK_EQ(fields[1], std::string("name2999"));
  CHECK(model.GetRow(1500, fields));
  CHECK_EQ(fields[1], std::string("name1500"));
  CHECK(!model.GetRow(3000, fields));
}

TEST(IndexerCanBeCancelled) {
  TempCSV file(ManyRows(200000));
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));

  CSVIndexer indexer;
  indexer.Start(file.path(), model.DataOffset(), CSVModel::kChunkSize,
                model.FileSize(), nullptr);
  indexer.Cancel();
  indexer.Join();

  const auto state = indexer.state();
  CHECK(state == CSVIndexer::State::Cancelled || state == CSVIndexer::State::Done);
  if (state == CSVIndexer::State::Cancelled) {
    CSVIndexer::Result result;
    CHECK(!indexer.Take(result)); // nothing to adopt from a cancelled scan
  }
}

TEST(IndexerReportsFailureOnMissingFile) {
  CSVIndexer indexer;
  indexer.Start("/definitely/not/here.csv", std::streampos(0),
                CSVModel::kChunkSize, 0, nullptr);
  indexer.Join();
  CHECK(indexer.state() == CSVIndexer::State::Failed);
}

// Searching must not be the thing that triggers a full-file scan.
TEST(SearchDoesNotForceARowCount) {
  TempCSV file(ManyRows(5000));
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));

  auto hit = model.FindNext("name42", 0, 0, false);
  CHECK(hit.has_value());
  CHECK_EQ(hit->row, size_t{42});
  CHECK(!model.RowCountIsExact());

  // A search that finds nothing walks to the end, which does establish the
  // count as a side effect, but it must still return cleanly.
  CHECK(!model.FindNext("absent-value", 0, 0, false).has_value());
}
