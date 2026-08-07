#include "test_util.h"

#include "csv_model.h"
#include "csv_system.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

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

// Sorting a huge file must refuse rather than exhaust memory. Since keys spill
// to disk, the only thing that has to fit is the resulting order.
TEST(SortIsRefusedWhenItWouldNotFit) {
  TempCSV file(ManyRows(5000));
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));

  const size_t rows = model.EstimatedRowCount();
  CHECK_EQ(model.EstimatedSortBytes(), rows * CSVModel::kSortOutputBytesPerRow);

  // Plenty of room: allowed.
  CHECK_EQ(model.CheckSortFeasible(4ull * 1024 * 1024 * 1024), std::string(""));
  // Not enough room even for the working buffer: refused, with the numbers.
  const std::string refusal = model.CheckSortFeasible(1024 * 1024);
  CHECK(!refusal.empty());
  CHECK(refusal.find("sorting") != std::string::npos);
  CHECK(refusal.find("filter") != std::string::npos);
  // Unknown memory must never block the user.
  CHECK_EQ(model.CheckSortFeasible(0), std::string(""));
}

// The point of spilling: a row count that used to be hopeless is now ordinary.
// 156M rows is roughly a 12 GB export.
TEST(AVeryLargeSortIsNoLongerRefusedOutright) {
  const size_t rows = 156000000;
  const size_t keys_in_memory = rows * CSVModel::kSortKeyBytesPerRow;
  const size_t answer_only = rows * CSVModel::kSortOutputBytesPerRow;

  // Holding every key would have wanted about nine gigabytes; the answer alone
  // wants a little over one.
  CHECK(keys_in_memory > 9ull * 1000 * 1000 * 1000);
  CHECK(answer_only < 1400ull * 1000 * 1000);

  // On a 4 GB budget the old requirement fails and the new one passes.
  const size_t budget = static_cast<size_t>(4ull * 1024 * 1024 * 1024 *
                                            CSVModel::kMemoryBudgetShare);
  CHECK(keys_in_memory > budget);
  CHECK(answer_only + CSVModel::kMinSortBufferBytes < budget);
}

TEST(SortBudgetIsWhatIsLeftAfterTheAnswer) {
  TempCSV file(ManyRows(5000));
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));

  // Roomy machine: the buffer is capped rather than allowed to grow forever.
  CHECK_EQ(model.SortMemoryBudget(64ull * 1024 * 1024 * 1024),
           CSVModel::kMaxSortBufferBytes);
  // Cramped machine: never below the floor, so a sort always makes progress.
  CHECK_EQ(model.SortMemoryBudget(1024), CSVModel::kMinSortBufferBytes);
  // The budget always leaves room for the order it is going to produce.
  const size_t available = 200u * 1024 * 1024;
  const size_t budget = model.SortMemoryBudget(available);
  const size_t share =
      static_cast<size_t>(static_cast<double>(available) *
                          CSVModel::kMemoryBudgetShare);
  CHECK(budget + model.EstimatedSortBytes() <= share ||
        budget == CSVModel::kMinSortBufferBytes);
}

// Sorting used to hold a key per row and so cost several times what filtering
// did. Spilling changed the shape of that: what a sort keeps is the answer,
// which is smaller per row than a filter's index, and what it adds instead is
// one fixed working buffer that does not grow with the file.
TEST(SortCostsAFixedBufferRatherThanMorePerRow) {
  CHECK(CSVModel::kSortOutputBytesPerRow < CSVModel::kFilterBytesPerRow);
  CHECK(CSVModel::kSortKeyBytesPerRow > CSVModel::kFilterBytesPerRow);

  TempCSV file(ManyRows(5000));
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
  CHECK(model.EstimatedSortBytes() < model.EstimatedFilterBytes());

  // On a small machine the fixed buffer is what a sort gets refused for, while
  // a filter of the same file still goes ahead.
  const size_t cramped = 4u * 1024 * 1024;
  CHECK(!model.CheckSortFeasible(cramped).empty());
  CHECK_EQ(model.CheckFilterFeasible(cramped), std::string(""));
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

// The background scan produces the same answer as counting inline, and the
// model reads correctly through the offsets it adopts. (The scanner itself is
// exercised in test_scan.cpp; this is about the handover.)
TEST(ModelAdoptsABackgroundIndex) {
  TempCSV file(ManyRows(3000));
  CSVModel model;
  CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));

  csvscan::Request request;
  model.DescribeScan(request);

  CSVScanner scanner;
  scanner.Start(request, nullptr);
  scanner.Join();
  CHECK(scanner.state() == CSVScanner::State::Done);

  CSVScanner::Result result;
  CHECK(scanner.Take(result));
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

// A view built in the background must behave exactly like one built inline.
TEST(ModelAdoptsABackgroundSort) {
  TempCSV file(ManyRows(3000));
  CSVModel inline_model;
  CHECK_EQ(inline_model.Open(file.path(), {}, {}), std::string(""));
  inline_model.SortByColumn(1, false);

  CSVModel background;
  CHECK_EQ(background.Open(file.path(), {}, {}), std::string(""));

  csvscan::Request request;
  background.DescribeScan(request);
  request.sort = true;
  request.sort_column = 1;
  request.want_order = true;

  CSVScanner scanner;
  scanner.Start(request, nullptr);
  scanner.Join();
  CSVScanner::Result result;
  CHECK(scanner.Take(result));

  CSVModel::ViewState state;
  state.sort_active = true;
  state.sort_column = 1;
  background.AdoptIndex(std::move(result.offsets), result.total_rows);
  background.AdoptView(state, std::move(result.order), result.has_order);

  CHECK_EQ(background.RowCount(), inline_model.RowCount());
  std::vector<std::string> here, there;
  for (size_t row : {size_t{0}, size_t{1}, size_t{1500}, size_t{2999}}) {
    CHECK(background.GetRow(row, here));
    CHECK(inline_model.GetRow(row, there));
    CHECK(here == there);
  }
  CHECK(background.sort_active());
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
