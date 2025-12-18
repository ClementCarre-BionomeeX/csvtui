#pragma once

#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <optional>

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
  size_t RowCount();
  bool RowCountKnown() const { return row_count_known_; }
  std::optional<size_t> FindNext(const std::string &pattern, size_t start_row);

private:
  std::ifstream file_;
  std::string file_path_;
  std::streampos data_offset_{0};
  char delimiter_ = ',';
  bool has_header_ = true;
  size_t row_count_ = 0;
  bool row_count_known_ = false;

  const size_t chunk_size_ = 512;
  std::map<size_t, std::vector<std::vector<std::string>>> chunk_cache_;
  std::vector<std::streampos> chunk_offsets_;
  size_t last_known_row_ = 0;

  std::vector<std::vector<std::string>> row_cache_;
  size_t current_start_row_ = 0;
  size_t current_row_count_ = 0;

  std::vector<std::string> header_;

  void DetectDelimiter(const std::string &line);
  std::vector<std::string> SplitLine(const std::string &line);

  void LoadRows();
  bool LoadChunk(size_t chunk_idx);
  std::streampos ResolveOffset(size_t chunk_idx);
  void EnsureOffsetsUpTo(size_t chunk_idx);
  size_t ComputeRowCount();
  bool RowMatches(const std::vector<std::string> &row,
                  const std::string &pattern) const;
};
