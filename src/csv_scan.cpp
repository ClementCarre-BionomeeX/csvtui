#include "csv_scan.h"

#include "csv_parser.h"
#include "csv_sortrun.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace csvscan {
namespace {

// How often the pass reports progress. Checking the clock every row would be
// its own cost, so the row count is a cheap gate on doing so; the clock then
// decides. Reporting on rows alone made the readout jump on a fast file and
// sit still on a slow one, which is the opposite of what it is for.
constexpr size_t kRowsBetweenClockChecks = 4096;
constexpr auto kReportInterval = std::chrono::milliseconds(100);

using csvsort::Key;

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

  std::vector<size_t> kept; // rows surviving the filter, in file order
  std::vector<Key> keys;    // one per row of the sorted view, until it spills
  size_t kept_count = 0;    // counted separately: `kept` may not be in use

  const csvsort::Order order{request.sort_descending};
  csvsort::RunStore runs(csvsort::TempDirectory());
  size_t key_bytes = 0; // approximate footprint of `keys`

  // Size the key vector once rather than doubling it a dozen times: during a
  // doubling both buffers are live, and on a large file that copy is the peak
  // that decides whether the sort fits at all. The estimate need not be right
  // — too low merely restores the growth it was avoiding.
  //
  // Only without a filter, where the row count is also the key count. A filter
  // usually keeps a small fraction, and reserving the whole file for it would
  // waste far more than the growth it avoids. And never past the budget, which
  // is the whole point of having one.
  if (request.expected_rows > 0 && collecting_keys && !filtering) {
    size_t want = request.expected_rows;
    if (request.sort_memory_budget > 0)
      want = std::min(want, request.sort_memory_budget / sizeof(Key));
    keys.reserve(want);
  }

  double sum = 0.0;
  bool first_number = true;

  std::string record;
  std::string scratch; // reused by the field walker: no allocation per row
  std::string value;   // the extracted stats cell, likewise reused
  size_t rows = 0;
  size_t since_report = 0;
  const size_t chunk_size = std::max<size_t>(request.chunk_size, 1);

  auto last_report = std::chrono::steady_clock::now();

  const auto publish = [&](bool final) {
    if (!report)
      return;
    Progress progress;
    progress.rows = rows;
    progress.kept = kept_count;
    progress.phase = Phase::Reading;
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
    last_report = std::chrono::steady_clock::now();
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
        Key key;
        key.row = index;
        csv::ExtractField(record, request.delimiter, request.sort_column,
                          key.text, scratch);
        key.numeric = csv::ParseNumber(key.text, key.number);
        key_bytes += csvsort::KeyBytes(key);
        keys.push_back(std::move(key));

        // Buffer full: sort what we have, write it out, and start again. This
        // is what stops a sort's memory from following the file's size.
        if (request.sort_memory_budget > 0 &&
            key_bytes >= request.sort_memory_budget) {
          if (!runs.Spill(keys, order)) {
            out.error = runs.error();
            return Outcome::Failed;
          }
          key_bytes = 0;
        }
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

    if (since_report >= kRowsBetweenClockChecks) {
      since_report = 0;
      if (std::chrono::steady_clock::now() - last_report >= kReportInterval)
        publish(false);
    }
  }

  out.total_rows = rows;
  if (out.stats.numeric > 0)
    out.stats.mean = sum / static_cast<double>(out.stats.numeric);

  if (collecting_keys) {
    out.order.reserve(kept_count);
    if (runs.empty()) {
      // Everything fit: no spilling, no merge, no temporary files.
      std::sort(keys.begin(), keys.end(), order);
      for (const Key &key : keys)
        out.order.push_back(key.row);
    } else {
      out.spilled_runs = runs.run_count() + (keys.empty() ? 0 : 1);
      // Merging tens of millions of keys takes seconds of its own. Reporting
      // it separately is the difference between a progress bar and a program
      // that appears to have stopped at 100%.
      const size_t expected = kept_count;
      auto merge_report =
          report ? std::function<void(size_t)>([&](size_t merged) {
            // Same rate limit as the read. The merge offers a tick every few
            // thousand rows, which on a large sort is hundreds a second — far
            // more redraws than anyone can see, and each one wakes the UI.
            const auto now = std::chrono::steady_clock::now();
            if (merged != expected && now - last_report < kReportInterval)
              return;
            last_report = now;

            Progress progress;
            progress.rows = rows;
            progress.kept = merged;
            progress.phase = Phase::Merging;
            progress.fraction =
                expected == 0 ? 1.0
                              : std::min(1.0, static_cast<double>(merged) /
                                                  static_cast<double>(expected));
            report(progress);
          })
                 : std::function<void(size_t)>();

      if (!runs.Merge(keys, order, out.order, cancelled, merge_report)) {
        if (!runs.error().empty()) {
          out.error = runs.error();
          return Outcome::Failed;
        }
        return Outcome::Cancelled;
      }
    }
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
  error_.clear();
  phase_.store(csvscan::Phase::Reading, std::memory_order_relaxed);
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
        phase_.store(progress.phase, std::memory_order_relaxed);
        if (notify)
          notify();
      });

  if (outcome == csvscan::Outcome::Done) {
    std::lock_guard<std::mutex> lock(result_mutex_);
    result_ = std::move(scanned);
  } else {
    // Written before the state is published, so a reader that sees Failed
    // sees the reason too.
    error_ = std::move(scanned.error);
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
