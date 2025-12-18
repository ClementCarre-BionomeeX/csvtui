#pragma once

#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class CSVModel {
public:
  CSVModel();

  bool Open(const std::string &path);
  void Close();

  void SetViewport(size_t start_row, size_t row_count, bool read_to_end = false);
  std::vector<std::vector<std::string>> GetVisibleRows();
  std::vector<std::string> GetHeader();

  char delimiter() const { return delimiter_; }
  bool is_open() const { return file_.is_open(); }
  size_t RowCount() const;
  bool RowCountKnown() const;
  size_t LastKnownRow() const;

private:
  struct Chunk {
    size_t start_row = 0;
    std::vector<std::vector<std::string>> rows;
  };

  const size_t chunk_size_ = 512;
  const size_t max_chunks_ = 8;

  std::ifstream file_;
  std::string file_path_;
  std::streampos data_offset_{0};
  char delimiter_ = ',';
  bool has_header_ = true;
  size_t row_count_ = 0;      // valid when row_count_known_ is true
  size_t last_known_row_ = 0; // highest row index observed so far
  bool row_count_known_ = false;

  std::vector<std::string> header_;

  // Viewport
  size_t current_start_row_ = 0;
  size_t current_row_count_ = 0;

  // Chunk cache and worker coordination
  std::map<size_t, std::shared_ptr<Chunk>> chunk_cache_;
  std::deque<size_t> lru_;
  std::vector<std::streampos> chunk_offsets_;

  struct Request {
    size_t start_row = 0;
    size_t row_count = 0;
    bool to_end = false;
  };
  std::optional<Request> pending_request_;
  bool stop_worker_ = false;
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;

  void DetectDelimiter(const std::string &line);
  std::vector<std::string> SplitLine(const std::string &line);

  void StartWorker();
  void StopWorker();
  void WorkerLoop();
  void EnsureRequest(size_t start_row, size_t row_count, bool to_end = false);
  void LoadChunksForRange(size_t start_row, size_t row_count);
  bool LoadChunk(size_t chunk_idx);
  void CacheChunk(size_t chunk_idx, std::vector<std::vector<std::string>> rows);
  void TouchLRU(size_t chunk_idx);
  void EvictIfNeeded();
  std::streampos ResolveOffset(size_t chunk_idx);
  void LoadToEnd();
  void NotifyDataReady();
};
