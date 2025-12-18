#include "csv_model.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <optional>

CSVModel::CSVModel() {}

bool CSVModel::Open(const std::string &path) {
  Close();

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

  data_offset_ = file_.tellg();
  chunk_offsets_.clear();
  chunk_offsets_.push_back(data_offset_);
  chunk_cache_.clear();
  row_cache_.clear();
  row_count_known_ = false;
  row_count_ = 0;
  last_known_row_ = 0;

  return true;
}

void CSVModel::Close() {
  if (file_.is_open())
    file_.close();
  chunk_cache_.clear();
  chunk_offsets_.clear();
  row_cache_.clear();
  header_.clear();
  row_count_ = 0;
  row_count_known_ = false;
  last_known_row_ = 0;
  current_start_row_ = 0;
  current_row_count_ = 0;
}

void CSVModel::SetViewport(size_t start_row, size_t row_count) {
  current_start_row_ = start_row;
  current_row_count_ = row_count;
  LoadRows();
}

std::vector<std::vector<std::string>> CSVModel::GetVisibleRows() {
  std::vector<std::vector<std::string>> rows;
  if (has_header_ && current_start_row_ == 0)
    rows.push_back(header_);

  for (const auto &row : row_cache_)
    rows.push_back(row);

  return rows;
}

std::vector<std::string> CSVModel::GetHeader() { return header_; }

size_t CSVModel::RowCount() {
  if (row_count_known_)
    return row_count_;
  return ComputeRowCount();
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

void CSVModel::LoadRows() {
  if (!file_.is_open())
    return;

  row_cache_.clear();

  const size_t start_chunk = current_start_row_ / chunk_size_;
  const size_t end_row = current_start_row_ + current_row_count_;
  const size_t end_chunk = end_row / chunk_size_;

  for (size_t chunk_idx = start_chunk; chunk_idx <= end_chunk; ++chunk_idx) {
    if (!LoadChunk(chunk_idx))
      break;
  }

  for (size_t row = current_start_row_; row < end_row; ++row) {
    size_t chunk_idx = row / chunk_size_;
    auto it = chunk_cache_.find(chunk_idx);
    if (it == chunk_cache_.end())
      break;
    const auto &chunk = it->second;
    size_t idx_in_chunk = row - chunk_idx * chunk_size_;
    if (idx_in_chunk >= chunk.size())
      break;
    row_cache_.push_back(chunk[idx_in_chunk]);
  }
}

bool CSVModel::LoadChunk(size_t chunk_idx) {
  if (chunk_cache_.count(chunk_idx))
    return true;

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
  size_t row_number = chunk_idx * chunk_size_;
  for (size_t i = 0; i < chunk_size_; ++i) {
    if (!std::getline(file_, line)) {
      row_count_known_ = true;
      row_count_ = row_number;
      last_known_row_ = row_number;
      break;
    }
    rows.push_back(SplitLine(line));
    ++row_number;
  }

  std::streampos next_offset = file_.tellg();
  if (next_offset != std::streampos(-1)) {
    while (chunk_offsets_.size() <= chunk_idx + 1)
      chunk_offsets_.push_back(next_offset);
  }

  last_known_row_ = std::max(last_known_row_, row_number);
  chunk_cache_[chunk_idx] = std::move(rows);
  return true;
}

std::streampos CSVModel::ResolveOffset(size_t chunk_idx) {
  if (chunk_idx < chunk_offsets_.size())
    return chunk_offsets_[chunk_idx];

  EnsureOffsetsUpTo(chunk_idx);
  if (chunk_idx < chunk_offsets_.size())
    return chunk_offsets_[chunk_idx];
  return std::streampos(-1);
}

void CSVModel::EnsureOffsetsUpTo(size_t target_chunk) {
  if (!file_.is_open())
    return;

  size_t current_chunk = chunk_offsets_.size() - 1;
  if (current_chunk >= target_chunk)
    return;

  std::streampos offset = chunk_offsets_.back();
  file_.clear();
  file_.seekg(offset);
  if (!file_)
    return;

  std::string line;
  size_t rows_seen = current_chunk * chunk_size_;

  while (current_chunk < target_chunk) {
    size_t lines = 0;
    for (; lines < chunk_size_ && std::getline(file_, line); ++lines) {
      ++rows_seen;
    }
    ++current_chunk;
    offset = file_.tellg();
    if (offset != std::streampos(-1))
      chunk_offsets_.push_back(offset);
    if (lines < chunk_size_) {
      row_count_known_ = true;
      row_count_ = rows_seen;
      last_known_row_ = rows_seen;
      break;
    }
  }
}

size_t CSVModel::ComputeRowCount() {
  if (!file_.is_open())
    return 0;

  // Start scanning from the last known chunk offset.
  EnsureOffsetsUpTo(chunk_offsets_.size());
  std::streampos offset = chunk_offsets_.empty() ? data_offset_ : chunk_offsets_.back();
  size_t rows = (chunk_offsets_.size() - 1) * chunk_size_;

  file_.clear();
  file_.seekg(offset);
  if (!file_)
    return last_known_row_;

  std::string line;
  while (std::getline(file_, line))
    ++rows;

  row_count_known_ = true;
  row_count_ = rows;
  last_known_row_ = rows;
  return row_count_;
}

std::optional<CSVModel::SearchHit>
CSVModel::FindNext(const std::string &pattern, size_t start_row) {
  if (!file_.is_open() || pattern.empty())
    return std::nullopt;

  const size_t total_rows = RowCountKnown() ? RowCount() : last_known_row_;
  size_t row = start_row;
  while (true) {
    size_t chunk_idx = row / chunk_size_;
    if (!LoadChunk(chunk_idx))
      break;
    auto it = chunk_cache_.find(chunk_idx);
    if (it == chunk_cache_.end())
      break;
    const auto &chunk = it->second;
    for (size_t i = row % chunk_size_; i < chunk.size(); ++i) {
      for (size_t col = 0; col < chunk[i].size(); ++col) {
        auto pos = chunk[i][col].find(pattern);
        if (pos != std::string::npos)
          return SearchHit{chunk_idx * chunk_size_ + i, col, pos};
      }
      ++row;
    }
    // If we've reached the known end and didn't find it, stop.
    if (RowCountKnown() && row >= RowCount())
      break;
  }

  // If we didn't know the total and didn't find it, finish scanning to end once.
  if (!RowCountKnown()) {
    ComputeRowCount();
  }

  return std::nullopt;
}

std::optional<CSVModel::SearchHit>
CSVModel::FindPrev(const std::string &pattern, size_t start_row) {
  if (!file_.is_open() || pattern.empty())
    return std::nullopt;

  size_t row = start_row;
  if (!RowCountKnown()) {
    ComputeRowCount();
  }
  size_t total = RowCount();
  if (total == 0)
    return std::nullopt;
  if (row >= total)
    row = total - 1;

  while (true) {
    size_t chunk_idx = row / chunk_size_;
    if (!LoadChunk(chunk_idx))
      break;
    auto it = chunk_cache_.find(chunk_idx);
    if (it == chunk_cache_.end())
      break;
    const auto &chunk = it->second;
    size_t idx_in_chunk = row % chunk_size_;
    for (size_t i = idx_in_chunk + 1; i-- > 0;) {
      if (i >= chunk.size())
        continue;
      for (size_t col = 0; col < chunk[i].size(); ++col) {
        auto pos = chunk[i][col].rfind(pattern);
        if (pos != std::string::npos)
          return SearchHit{chunk_idx * chunk_size_ + i, col, pos};
      }
      if (chunk_idx == 0 && i == 0)
        return std::nullopt;
    }
    if (chunk_idx == 0)
      break;
    row = chunk_idx * chunk_size_ - 1;
  }

  return std::nullopt;
}
