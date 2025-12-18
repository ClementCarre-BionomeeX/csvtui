
#include "csv_model.h"
#include <algorithm>
#include <iostream>
#include <sstream>

CSVModel::CSVModel() {}

bool CSVModel::Open(const std::string &path) {
  Close(); // Close any previously open file

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
  row_count_ = 0;
  std::string line;
  while (std::getline(file_, line))
    ++row_count_;

  // Reset to beginning for future reads.
  file_.clear();
  file_.seekg(0);

  return true;
}

void CSVModel::Close() {
  if (file_.is_open())
    file_.close();
  row_cache_.clear();
  current_start_row_ = 0;
  current_row_count_ = 0;
  header_.clear();
  row_count_ = 0;
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

  file_.clear();
  file_.seekg(0);
  std::string line;

  // Skip header line
  std::getline(file_, line);

  size_t current = 0;
  row_cache_.clear();

  while (std::getline(file_, line)) {
    if (current >= current_start_row_) {
      row_cache_.push_back(SplitLine(line));
      if (row_cache_.size() >= current_row_count_)
        break;
    }
    ++current;
  }
}
