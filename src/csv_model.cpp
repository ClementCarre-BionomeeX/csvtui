#include "csv_model.h"
#include <algorithm>
#include <iostream>
#include <sstream>

#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

CSVModel::CSVModel() {}

bool CSVModel::Open(const std::string &path) {
  Close(); // Close any previously open file and worker

  file_path_ = path;
  file_.open(path);
  if (!file_.is_open())
    return false;

  std::string first_line;
  std::getline(file_, first_line);
  if (first_line.empty()) {
    Close();
    return false;
  }

  DetectDelimiter(first_line);
  header_ = SplitLine(first_line);

  // Remember where the data starts (right after header).
  data_offset_ = file_.tellg();
  chunk_offsets_.clear();
  chunk_offsets_.push_back(data_offset_);

  last_known_row_ = 0;
  row_count_ = 0;
  row_count_known_ = false;
  chunk_cache_.clear();
  lru_.clear();

  StartWorker();
  return true;
}

void CSVModel::Close() {
  StopWorker();
  if (file_.is_open())
    file_.close();
  std::lock_guard<std::mutex> lock(mutex_);
  chunk_cache_.clear();
  lru_.clear();
  header_.clear();
  row_count_ = 0;
  last_known_row_ = 0;
  row_count_known_ = false;
  current_start_row_ = 0;
  current_row_count_ = 0;
}

void CSVModel::StartWorker() {
  stop_worker_ = false;
  worker_ = std::thread([this] { WorkerLoop(); });
  // Kick off an initial request for the top of the file.
  EnsureRequest(0, current_row_count_ == 0 ? chunk_size_ : current_row_count_);
}

void CSVModel::StopWorker() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_worker_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable())
    worker_.join();
}

void CSVModel::SetViewport(size_t start_row, size_t row_count, bool read_to_end) {
  current_start_row_ = start_row;
  current_row_count_ = row_count;
  EnsureRequest(start_row, row_count, read_to_end);
}

std::vector<std::vector<std::string>> CSVModel::GetVisibleRows() {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::vector<std::string>> rows;
  size_t start = current_start_row_;
  size_t end = start + current_row_count_;

  const size_t known_row_count = row_count_known_ ? row_count_ : last_known_row_;

  for (size_t row_index = start; row_index < end; ++row_index) {
    if (row_count_known_ && row_index >= row_count_)
      break;

    size_t chunk_idx = row_index / chunk_size_;
    auto it = chunk_cache_.find(chunk_idx);
    if (it != chunk_cache_.end()) {
      TouchLRU(chunk_idx);
      const auto &chunk_rows = it->second->rows;
      size_t in_chunk = row_index - chunk_idx * chunk_size_;
      if (in_chunk < chunk_rows.size())
        rows.push_back(chunk_rows[in_chunk]);
      else if (row_count_known_) // past EOF in a partial chunk
        break;
      else
        rows.push_back({"... loading ..."});
    } else {
      // If we are beyond what we have seen, avoid infinite placeholders.
      if (!row_count_known_ && row_index > known_row_count + chunk_size_)
        break;
      rows.push_back({"... loading ..."});
    }
  }

  return rows;
}

std::vector<std::string> CSVModel::GetHeader() { return header_; }

size_t CSVModel::RowCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return row_count_known_ ? row_count_ : last_known_row_;
}

bool CSVModel::RowCountKnown() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return row_count_known_;
}

size_t CSVModel::LastKnownRow() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_known_row_;
}

void CSVModel::DetectDelimiter(const std::string &line) {
  std::vector<char> candidates = {',', '\t', ';', '|'};
  size_t best_score = 0;
  char best_delim = ',';

  for (char c : candidates) {
    size_t count = std::count(line.begin(), line.end(), c);
    if (count > best_score) {
      best_score = count;
      best_delim = c;
    }
  }

  delimiter_ = best_delim;
}

std::vector<std::string> CSVModel::SplitLine(const std::string &line) {
  std::vector<std::string> tokens;
  std::string token;
  std::istringstream stream(line);

  while (std::getline(stream, token, delimiter_)) {
    tokens.push_back(token);
  }

  return tokens;
}

void CSVModel::WorkerLoop() {
  while (true) {
    Request request;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return stop_worker_ || pending_request_.has_value(); });
      if (stop_worker_)
        break;
      request = *pending_request_;
      pending_request_.reset();
    }

    LoadChunksForRange(request.start_row, request.row_count);
    if (request.to_end)
      LoadToEnd();
  }
}

void CSVModel::EnsureRequest(size_t start_row, size_t row_count, bool to_end) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_request_ = Request{start_row, row_count, to_end};
  }
  cv_.notify_one();
}

void CSVModel::LoadChunksForRange(size_t start_row, size_t row_count) {
  size_t start_chunk = start_row / chunk_size_;
  size_t end_row = start_row + row_count;
  size_t end_chunk = end_row / chunk_size_;

  for (size_t chunk_idx = start_chunk; chunk_idx <= end_chunk; ++chunk_idx) {
    LoadChunk(chunk_idx);
  }
}

bool CSVModel::LoadChunk(size_t chunk_idx) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (chunk_cache_.count(chunk_idx)) {
      TouchLRU(chunk_idx);
      return true;
    }
  }

  if (!file_.is_open())
    return false;

  std::streampos offset = ResolveOffset(chunk_idx);
  if (offset == std::streampos(-1))
    return false;

  file_.clear();
  file_.seekg(offset);
  if (!file_)
    return false;

  std::vector<std::vector<std::string>> rows;
  rows.reserve(chunk_size_);

  std::string line;
  size_t row = chunk_idx * chunk_size_;
  for (size_t i = 0; i < chunk_size_; ++i) {
    if (!std::getline(file_, line)) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        row_count_ = row;
        row_count_known_ = true;
        last_known_row_ = std::max(last_known_row_, row);
      }
      NotifyDataReady();
      break;
    }
    rows.push_back(SplitLine(line));
    ++row;
  }

  // Record offset for the next chunk boundary if possible.
  std::streampos next_offset = file_.tellg();
  if (next_offset != std::streampos(-1)) {
    while (chunk_offsets_.size() <= chunk_idx + 1)
      chunk_offsets_.push_back(next_offset);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    last_known_row_ = std::max(last_known_row_, chunk_idx * chunk_size_ + rows.size());
  }

  CacheChunk(chunk_idx, std::move(rows));
  NotifyDataReady();
  return true;
}

void CSVModel::CacheChunk(size_t chunk_idx,
                          std::vector<std::vector<std::string>> rows) {
  auto chunk = std::make_shared<Chunk>();
  chunk->start_row = chunk_idx * chunk_size_;
  chunk->rows = std::move(rows);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    chunk_cache_[chunk_idx] = chunk;
    TouchLRU(chunk_idx);
  }
  EvictIfNeeded();
}

void CSVModel::TouchLRU(size_t chunk_idx) {
  auto it = std::find(lru_.begin(), lru_.end(), chunk_idx);
  if (it != lru_.end())
    lru_.erase(it);
  lru_.push_front(chunk_idx);
}

void CSVModel::EvictIfNeeded() {
  std::lock_guard<std::mutex> lock(mutex_);
  while (chunk_cache_.size() > max_chunks_) {
    size_t victim = lru_.back();
    lru_.pop_back();
    chunk_cache_.erase(victim);
  }
}

std::streampos CSVModel::ResolveOffset(size_t chunk_idx) {
  // We already know this offset.
  if (chunk_idx < chunk_offsets_.size())
    return chunk_offsets_[chunk_idx];

  if (!file_.is_open())
    return std::streampos(-1);

  // Start from the last known offset and advance until we reach the desired
  // chunk boundary, recording offsets as we go.
  size_t current_chunk = chunk_offsets_.size() - 1;
  std::streampos offset = chunk_offsets_.back();
  file_.clear();
  file_.seekg(offset);
  if (!file_)
    return std::streampos(-1);

  std::string line;
  size_t rows_seen = current_chunk * chunk_size_;
  while (current_chunk < chunk_idx) {
    size_t lines = 0;
    for (; lines < chunk_size_ && std::getline(file_, line); ++lines) {
      ++rows_seen;
    }
    ++current_chunk;
    offset = file_.tellg();
    chunk_offsets_.push_back(offset);
    if (!file_) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        row_count_ = rows_seen;
        row_count_known_ = true;
        last_known_row_ = std::max(last_known_row_, rows_seen);
      }
      NotifyDataReady();
      break;
    }
  }
  return (chunk_idx < chunk_offsets_.size()) ? chunk_offsets_[chunk_idx]
                                             : std::streampos(-1);
}

void CSVModel::LoadToEnd() {
  size_t start_chunk = RowCount() / chunk_size_;
  while (true) {
    bool loaded = LoadChunk(start_chunk);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (row_count_known_)
        break;
    }
    if (!loaded)
      break;
    ++start_chunk;
  }
}

void CSVModel::NotifyDataReady() {
  if (auto *screen = ftxui::ScreenInteractive::Active()) {
    screen->PostEvent(ftxui::Event::Custom);
  }
}
