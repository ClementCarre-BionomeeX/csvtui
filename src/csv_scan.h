#pragma once

#include <atomic>
#include <cstddef>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// One streaming pass over a CSV file.
//
// Everything that has to look at every row goes through here: counting,
// filtering, sorting and column statistics. They are the same pass with
// different things accumulated along the way, so asking for a sort also yields
// the exact row count and the chunk offset table for free.
//
// The pass is written once, as csvscan::Run, and can be driven either on the
// calling thread (what CSVModel does, and what the tests exercise) or on a
// worker thread by CSVScanner. There is deliberately only one copy of the
// logic: a background sort that disagreed with a foreground one would be a
// miserable bug to find.
namespace csvscan {

struct Stats {
  size_t total = 0;
  size_t empty = 0;
  size_t numeric = 0;
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
};

// Describes the view to produce, not the operation to perform: the scanner is
// handed the state the user wants and works out the single pass that reaches
// it. That is what lets "sort while filtered" stay one read of the file.
struct Request {
  std::string path;
  std::streampos data_offset{0};
  long long file_size = 0;
  char delimiter = ',';
  size_t chunk_size = 512;

  bool filter = false;
  std::string filter_pattern;

  bool sort = false;
  size_t sort_column = 0;
  bool sort_descending = false;

  // Build the view->physical index. False for a pass that only needs the row
  // count or the statistics, which leaves the current ordering alone.
  bool want_order = false;

  bool want_stats = false;
  size_t stats_column = 0;

  // Count rows that match this as well as the filter, without building an
  // ordering. Used to answer "how many matches" after a search: the search
  // itself stops at the first hit, so the total has to come from somewhere,
  // and it can arrive late rather than not at all.
  std::string count_pattern;

  // Roughly how many rows the file holds, used to size the key vector up
  // front. Growing it instead costs a doubling: the old and new buffers are
  // both live during the copy, which on a large file is the peak that decides
  // whether the sort fits in memory at all. Zero means "no idea", which is
  // safe but leaves that spike in place.
  size_t expected_rows = 0;

  // How much memory the sort may spend on keys before writing a sorted run to
  // a temporary file and starting a fresh buffer. This is what decouples the
  // cost of a sort from the size of the file. Zero keeps every key in memory,
  // which is right for small files and for the tests.
  size_t sort_memory_budget = 0;
};

struct Result {
  std::vector<std::streampos> offsets;
  size_t total_rows = 0;
  // Physical row indices in view order. Empty when the view is the file in its
  // own order, which the model stores as "no index at all".
  std::vector<size_t> order;
  bool has_order = false;
  Stats stats;
  // Rows matching `count_pattern`, within the filter if there was one.
  size_t matches = 0;
  // How many sorted runs the sort had to spill. Zero means it fit in memory.
  size_t spilled_runs = 0;
  // Set when the outcome is Failed, saying what went wrong. Running out of
  // temporary disk space is the interesting case, and a bare "sort failed"
  // would leave nowhere to go.
  std::string error;
};

// Which part of the work is running. A large sort reads the file and then
// merges what it spilled, and the merge is not instant — leaving the readout
// at "100%" through it is exactly the silence this reports away.
enum class Phase { Reading, Merging };

struct Progress {
  size_t rows = 0;     // records read so far
  size_t kept = 0;     // of those, how many the filter accepted
  double fraction = 0; // 0..1, within the current phase
  Phase phase = Phase::Reading;
};

enum class Outcome { Done, Cancelled, Failed };

// Runs the pass on the calling thread. `cancelled` is polled once per row and
// `report` is called every few hundred thousand rows; either may be empty.
Outcome Run(const Request &request, Result &out,
            const std::function<bool()> &cancelled,
            const std::function<void(const Progress &)> &report);

} // namespace csvscan

// Drives csvscan::Run on a worker thread.
//
// The worker opens its own file handle and touches no model state; the result
// is handed over only when the UI thread calls Take(). The read path therefore
// needs no locks, and the UI stays live while a multi-gigabyte file is scanned.
class CSVScanner {
public:
  enum class State { Idle, Running, Done, Cancelled, Failed };

  using Request = csvscan::Request;
  using Result = csvscan::Result;
  using Stats = csvscan::Stats;

  CSVScanner() = default;
  ~CSVScanner();

  CSVScanner(const CSVScanner &) = delete;
  CSVScanner &operator=(const CSVScanner &) = delete;

  // `notify` is called from the worker thread whenever progress advances; use
  // it to wake the UI. It must be safe to call from another thread.
  void Start(Request request, std::function<void()> notify);
  void Cancel();
  void Join();

  State state() const { return state_.load(std::memory_order_acquire); }
  bool running() const { return state() == State::Running; }
  // 0..1, derived from bytes consumed.
  double progress() const { return progress_.load(std::memory_order_relaxed); }
  size_t rows_seen() const { return rows_seen_.load(std::memory_order_relaxed); }
  // Rows kept by the filter so far. Equals rows_seen() when not filtering.
  size_t rows_kept() const { return rows_kept_.load(std::memory_order_relaxed); }
  csvscan::Phase phase() const { return phase_.load(std::memory_order_relaxed); }

  // What this pass was asked for, so the UI can name it while it runs.
  const Request &request() const { return request_; }

  // Why the pass failed. Only meaningful once state() == Failed; safe to read
  // then, because it is written before the state is published.
  const std::string &error() const { return error_; }

  // Moves the finished result out. Only valid once state() == Done; returns
  // false otherwise. Leaves the scanner Idle.
  bool Take(Result &out);

private:
  void Run(Request request, std::function<void()> notify);

  std::thread worker_;
  std::atomic<State> state_{State::Idle};
  std::atomic<bool> cancel_{false};
  std::atomic<double> progress_{0.0};
  std::atomic<size_t> rows_seen_{0};
  std::atomic<size_t> rows_kept_{0};
  std::atomic<csvscan::Phase> phase_{csvscan::Phase::Reading};

  Request request_; // written before the worker starts, read-only after
  std::string error_; // written before state_ becomes Failed
  std::mutex result_mutex_;
  Result result_;
};
