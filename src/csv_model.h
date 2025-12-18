#pragma once

#include <deque>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

class CSVModel {
public:
  CSVModel();

  bool Open(const std::string &path);
  void Close();

  void SetViewport(size_t start_row, size_t row_count);
  std::vector<std::vector<std::string>> GetVisibleRows();
  std::vector<std::string> GetHeader();

  char delimiter() const { return delimiter_; }
  bool is_open() const { return file_.is_open(); }
  size_t RowCount() const { return row_count_; }

private:
  std::ifstream file_;
  std::string file_path_;
  char delimiter_ = ',';
  bool has_header_ = true;
  size_t row_count_ = 0;

  std::deque<std::vector<std::string>> row_cache_;
  size_t current_start_row_ = 0;
  size_t current_row_count_ = 0;

  std::vector<std::string> header_;

  void DetectDelimiter(const std::string &line);
  std::vector<std::string> SplitLine(const std::string &line);

  void LoadRows();
};
