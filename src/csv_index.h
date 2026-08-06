#pragma once

#include <atomic>
#include <functional>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Scans a CSV file on a worker thread to produce the exact row count and the
// chunk offset table, so that jumping around a very large file stops costing a
// full read every time.
//
// The worker opens its own file handle and touches no model state; the result
// is handed over only when the UI thread calls Take(). That keeps the whole
// thing free of locks on the read path.
class CSVIndexer {
public:
  enum class State { Idle, Running, Done, Cancelled, Failed };

  struct Result {
    std::vector<std::streampos> offsets;
    size_t total_rows = 0;
  };

  CSVIndexer() = default;
  ~CSVIndexer();

  CSVIndexer(const CSVIndexer &) = delete;
  CSVIndexer &operator=(const CSVIndexer &) = delete;

  // `notify` is called from the worker thread whenever progress advances; use
  // it to wake the UI. It must be safe to call from another thread.
  void Start(const std::string &path, std::streampos data_offset,
             size_t chunk_size, long long file_size,
             std::function<void()> notify);
  void Cancel();
  void Join();

  State state() const { return state_.load(std::memory_order_acquire); }
  bool running() const { return state() == State::Running; }
  // 0..1, derived from bytes consumed.
  double progress() const { return progress_.load(std::memory_order_relaxed); }
  size_t rows_seen() const { return rows_seen_.load(std::memory_order_relaxed); }

  // Moves the finished index out. Only valid once state() == Done; returns
  // false otherwise. Leaves the indexer Idle.
  bool Take(Result &out);

private:
  void Run(std::string path, std::streampos data_offset, size_t chunk_size,
           long long file_size, std::function<void()> notify);

  std::thread worker_;
  std::atomic<State> state_{State::Idle};
  std::atomic<bool> cancel_{false};
  std::atomic<double> progress_{0.0};
  std::atomic<size_t> rows_seen_{0};

  std::mutex result_mutex_;
  Result result_;
};
