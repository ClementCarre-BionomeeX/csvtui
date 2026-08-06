#include "csv_index.h"

#include "csv_parser.h"

#include <algorithm>

namespace {
// How often the worker publishes progress. Often enough to feel live, rarely
// enough that waking the UI is not the bottleneck.
constexpr size_t kNotifyEveryRows = 200000;
} // namespace

CSVIndexer::~CSVIndexer() {
  Cancel();
  Join();
}

void CSVIndexer::Start(const std::string &path, std::streampos data_offset,
                       size_t chunk_size, long long file_size,
                       std::function<void()> notify) {
  if (state() == State::Running)
    return;
  Join(); // reap a previous run

  cancel_.store(false, std::memory_order_release);
  progress_.store(0.0, std::memory_order_relaxed);
  rows_seen_.store(0, std::memory_order_relaxed);
  state_.store(State::Running, std::memory_order_release);

  worker_ = std::thread(&CSVIndexer::Run, this, path, data_offset, chunk_size,
                        file_size, std::move(notify));
}

void CSVIndexer::Cancel() { cancel_.store(true, std::memory_order_release); }

void CSVIndexer::Join() {
  if (worker_.joinable())
    worker_.join();
}

bool CSVIndexer::Take(Result &out) {
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

void CSVIndexer::Run(std::string path, std::streampos data_offset,
                     size_t chunk_size, long long file_size,
                     std::function<void()> notify) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    state_.store(State::Failed, std::memory_order_release);
    if (notify)
      notify();
    return;
  }

  file.seekg(data_offset);
  if (!file) {
    state_.store(State::Failed, std::memory_order_release);
    if (notify)
      notify();
    return;
  }

  Result scanned;
  scanned.offsets.push_back(data_offset);

  std::string record;
  size_t rows = 0;
  size_t since_notify = 0;

  while (true) {
    if (cancel_.load(std::memory_order_acquire)) {
      state_.store(State::Cancelled, std::memory_order_release);
      if (notify)
        notify();
      return;
    }

    if (!csv::ReadRecord(file, record))
      break;
    ++rows;
    ++since_notify;

    if (rows % chunk_size == 0) {
      const std::streampos here = file.tellg();
      if (here == std::streampos(-1))
        break;
      scanned.offsets.push_back(here);
    }

    if (since_notify >= kNotifyEveryRows) {
      since_notify = 0;
      rows_seen_.store(rows, std::memory_order_relaxed);
      const std::streampos here = file.tellg();
      if (file_size > 0 && here != std::streampos(-1)) {
        progress_.store(std::min(1.0, static_cast<double>(here) /
                                          static_cast<double>(file_size)),
                        std::memory_order_relaxed);
      }
      if (notify)
        notify();
    }
  }

  scanned.total_rows = rows;
  rows_seen_.store(rows, std::memory_order_relaxed);
  progress_.store(1.0, std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lock(result_mutex_);
    result_ = std::move(scanned);
  }
  state_.store(State::Done, std::memory_order_release);
  if (notify)
    notify();
}
