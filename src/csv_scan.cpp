#include "csv_scan.h"

#include "csv_parser.h"

#include <algorithm>
#include <utility>

namespace csvscan {
namespace {

// How often the pass reports progress. Often enough to feel live, rarely
// enough that waking the UI is not the bottleneck.
constexpr size_t kReportEveryRows = 200000;

// One row's sort key. The column value is extracted once so that comparison
// never touches the file, and the row number rides along so the permutation
// falls out of the sort itself rather than needing a second index array.
struct SortKey {
  std::string text;
  double number = 0.0;
  size_t row = 0;
  bool numeric = false;
};

// Ordering keys directly, rather than sorting an index that points at them.
//
// The row number is the final tiebreak, which makes this a total order and so
// makes plain std::sort produce exactly what std::stable_sort would. That
// matters for more than tidiness: stable_sort asks for a temporary buffer as
// large as the data, and on a file with tens of millions of rows that buffer
// was doubling the peak memory of a sort.
struct KeyOrder {
  bool descending = false;

  bool operator()(const SortKey &a, const SortKey &b) const {
    if (a.numeric != b.numeric)
      return a.numeric; // numbers before text, whichever way we are sorting
    if (a.numeric) {
      if (a.number != b.number)
        return descending ? b.number < a.number : a.number < b.number;
    } else if (a.text != b.text) {
      return descending ? b.text < a.text : a.text < b.text;
    }
    // Equal keys keep file order, so sorting by one column and then another
    // refines the result rather than scrambling it.
    return a.row < b.row;
  }
};

} // namespace

Outcome Run(const Request &request, Result &out,
            const std::function<bool()> &cancelled,
            const std::function<void(const Progress &)> &report) {
  out = Result{};

  std::ifstream file(request.path, std::ios::binary);
  if (!file.is_open())
    return Outcome::Failed;
  file.seekg(request.data_offset);
  if (!file)
    return Outcome::Failed;

  out.offsets.push_back(request.data_offset);

  const bool filtering = request.filter && !request.filter_pattern.empty();
  const bool ignore_case =
      filtering && csv::SmartCaseInsensitive(request.filter_pattern);

  // Only one of these is ever populated: a sort needs a key per row, a plain
  // filter needs just the row numbers, and a stats pass needs neither.
  const bool collecting_keys = request.want_order && request.sort;
  const bool collecting_rows = request.want_order && !request.sort && filtering;

  std::vector<size_t> kept;  // rows surviving the filter, in file order
  std::vector<SortKey> keys; // one per row of the sorted view
  size_t kept_count = 0;     // counted separately: `kept` may not be in use

  // Size the key vector once rather than doubling it a dozen times: during a
  // doubling both buffers are live, and on a large file that copy is the peak
  // that decides whether the sort fits at all. The estimate need not be right
  // — too low merely restores the growth it was avoiding.
  //
  // Only without a filter, where the row count is also the key count. A filter
  // usually keeps a small fraction, and reserving the whole file for it would
  // waste far more than the growth it avoids.
  if (request.expected_rows > 0 && collecting_keys && !filtering)
    keys.reserve(request.expected_rows);

  double sum = 0.0;
  bool first_number = true;

  std::string record;
  std::string scratch; // reused by the field walker: no allocation per row
  std::string value;   // the extracted stats cell, likewise reused
  size_t rows = 0;
  size_t since_report = 0;
  const size_t chunk_size = std::max<size_t>(request.chunk_size, 1);

  const auto publish = [&](bool final) {
    if (!report)
      return;
    Progress progress;
    progress.rows = rows;
    progress.kept = kept_count;
    if (final) {
      progress.fraction = 1.0;
    } else {
      const std::streampos here = file.tellg();
      if (request.file_size > 0 && here != std::streampos(-1)) {
        progress.fraction =
            std::min(1.0, static_cast<double>(here) /
                              static_cast<double>(request.file_size));
      }
    }
    report(progress);
  };

  while (true) {
    if (cancelled && cancelled())
      return Outcome::Cancelled;

    if (!csv::ReadRecord(file, record))
      break;
    const size_t index = rows;
    ++rows;
    ++since_report;

    if (rows % chunk_size == 0) {
      const std::streampos here = file.tellg();
      if (here == std::streampos(-1))
        break;
      out.offsets.push_back(here);
    }

    // Does this row belong to the view being built?
    const bool keep =
        !filtering || csv::RecordContains(record, request.delimiter,
                                          request.filter_pattern, ignore_case,
                                          scratch);

    if (keep) {
      ++kept_count;
      if (collecting_keys) {
        SortKey key;
        key.row = index;
        csv::ExtractField(record, request.delimiter, request.sort_column,
                          key.text, scratch);
        key.numeric = csv::ParseNumber(key.text, key.number);
        keys.push_back(std::move(key));
      } else if (collecting_rows) {
        kept.push_back(index);
      }

      if (request.want_stats) {
        ++out.stats.total;
        csv::ExtractField(record, request.delimiter, request.stats_column,
                          value, scratch);
        if (value.empty()) {
          ++out.stats.empty;
        } else {
          double number = 0.0;
          if (csv::ParseNumber(value, number)) {
            ++out.stats.numeric;
            sum += number;
            if (first_number) {
              out.stats.min = out.stats.max = number;
              first_number = false;
            } else {
              out.stats.min = std::min(out.stats.min, number);
              out.stats.max = std::max(out.stats.max, number);
            }
          }
        }
      }
    }

    if (since_report >= kReportEveryRows) {
      since_report = 0;
      publish(false);
    }
  }

  out.total_rows = rows;
  if (out.stats.numeric > 0)
    out.stats.mean = sum / static_cast<double>(out.stats.numeric);

  if (collecting_keys) {
    std::sort(keys.begin(), keys.end(), KeyOrder{request.sort_descending});
    out.order.reserve(keys.size());
    for (const SortKey &key : keys)
      out.order.push_back(key.row);
    keys.clear();
    keys.shrink_to_fit();
    out.has_order = true;
  } else if (collecting_rows) {
    out.order = std::move(kept);
    out.has_order = true;
  }
  // Neither sorting nor filtering: the view is the file in its own order, and
  // the model represents that as no index at all.

  publish(true);
  return Outcome::Done;
}

} // namespace csvscan

CSVScanner::~CSVScanner() {
  Cancel();
  Join();
}

void CSVScanner::Start(Request request, std::function<void()> notify) {
  if (state() == State::Running)
    return;
  Join(); // reap a previous run

  request_ = request;
  cancel_.store(false, std::memory_order_release);
  progress_.store(0.0, std::memory_order_relaxed);
  rows_seen_.store(0, std::memory_order_relaxed);
  rows_kept_.store(0, std::memory_order_relaxed);
  state_.store(State::Running, std::memory_order_release);

  worker_ =
      std::thread(&CSVScanner::Run, this, std::move(request), std::move(notify));
}

void CSVScanner::Cancel() { cancel_.store(true, std::memory_order_release); }

void CSVScanner::Join() {
  if (worker_.joinable())
    worker_.join();
}

bool CSVScanner::Take(Result &out) {
  if (state() != State::Done)
    return false;
  Join();
  {
    std::lock_guard<std::mutex> lock(result_mutex_);
    out = std::move(result_);
    result_ = Result{};
  }
  state_.store(State::Idle, std::memory_order_release);
  return true;
}

void CSVScanner::Run(Request request, std::function<void()> notify) {
  Result scanned;
  const csvscan::Outcome outcome = csvscan::Run(
      request, scanned, [this] { return cancel_.load(std::memory_order_acquire); },
      [this, &notify](const csvscan::Progress &progress) {
        rows_seen_.store(progress.rows, std::memory_order_relaxed);
        rows_kept_.store(progress.kept, std::memory_order_relaxed);
        progress_.store(progress.fraction, std::memory_order_relaxed);
        if (notify)
          notify();
      });

  if (outcome == csvscan::Outcome::Done) {
    std::lock_guard<std::mutex> lock(result_mutex_);
    result_ = std::move(scanned);
  }

  switch (outcome) {
  case csvscan::Outcome::Done:
    state_.store(State::Done, std::memory_order_release);
    break;
  case csvscan::Outcome::Cancelled:
    state_.store(State::Cancelled, std::memory_order_release);
    break;
  case csvscan::Outcome::Failed:
    state_.store(State::Failed, std::memory_order_release);
    break;
  }
  if (notify)
    notify();
}
