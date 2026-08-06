#pragma once

#include <fstream>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Streams a CSV file from disk, keeping only a bounded window of parsed rows in
// memory. Row indices handed to and returned from this class are *view*
// indices: when a sort or filter is active they are positions in the reordered
// view, not physical line numbers.
class CSVModel {
public:
  struct SearchHit {
    size_t row = 0;
    size_t col = 0;
    size_t pos = 0; // byte offset of the match inside the cell
    size_t len = 0;
  };

  struct ColumnStats {
    size_t total = 0;
    size_t empty = 0;
    size_t numeric = 0;
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
  };

  CSVModel() = default;
  ~CSVModel();

  CSVModel(const CSVModel &) = delete;
  CSVModel &operator=(const CSVModel &) = delete;

  // Returns an empty string on success, otherwise a human-readable reason.
  std::string Open(const std::string &path, std::optional<char> delimiter,
                   std::optional<bool> has_header);
  void Close();

  bool is_open() const { return file_.is_open(); }
  char delimiter() const { return delimiter_; }
  bool has_header() const { return has_header_; }
  void SetHasHeader(bool value);
  const std::string &path() const { return display_path_; }
  // Overrides the name shown in the status bar (used for piped input, where
  // the real path is an unhelpful temporary file).
  void SetDisplayName(const std::string &name) { display_path_ = name; }
  const std::vector<std::string> &Header() const { return header_; }
  size_t ColumnCount() const { return column_count_; }

  // Number of rows in the current view. Cheap once known; may trigger a scan.
  size_t RowCount();
  bool RowCountKnown() const;
  size_t TotalRowCount(); // ignores any active filter

  bool GetRow(size_t view_index, std::vector<std::string> &out);
  std::vector<std::vector<std::string>> GetRows(size_t start, size_t count);

  const std::vector<int> &ColumnWidths() const { return column_widths_; }
  bool ColumnIsNumeric(size_t col) const;
  std::string ColumnName(size_t col) const;

  std::optional<SearchHit> FindNext(const std::string &pattern, size_t row,
                                    size_t col, bool wrap);
  std::optional<SearchHit> FindPrev(const std::string &pattern, size_t row,
                                    size_t col, bool wrap);

  // Ordering and filtering both work by building a view->physical index map.
  void SortByColumn(size_t col, bool descending);
  void ClearSort();
  bool sort_active() const { return sort_active_; }
  size_t sort_column() const { return sort_column_; }
  bool sort_descending() const { return sort_descending_; }

  size_t ApplyFilter(const std::string &pattern);
  void ClearFilter();
  bool filter_active() const { return filter_active_; }
  const std::string &filter_pattern() const { return filter_pattern_; }

  ColumnStats ComputeColumnStats(size_t col);

  // Physical row count of the file, scanning it fully if needed.
  size_t EnsureTotalRowCount();

private:
  static constexpr size_t kChunkSize = 512;
  static constexpr size_t kMaxCachedChunks = 48; // ~24k rows resident
  static constexpr size_t kSampleRows = 1000;
  static constexpr int kMaxSampledWidth = 48;

  std::ifstream file_;
  std::string display_path_;
  std::streampos data_offset_{0};
  char delimiter_ = ',';
  bool has_header_ = true;
  std::vector<std::string> header_;
  size_t column_count_ = 0;

  size_t total_rows_ = 0;
  bool total_rows_known_ = false;

  std::vector<std::streampos> chunk_offsets_;
  std::unordered_map<size_t, std::vector<std::vector<std::string>>> chunk_cache_;
  std::list<size_t> lru_;
  std::unordered_map<size_t, std::list<size_t>::iterator> lru_pos_;

  std::vector<int> column_widths_;
  std::vector<bool> column_numeric_;

  std::vector<size_t> order_; // view index -> physical index; empty = identity
  bool sort_active_ = false;
  size_t sort_column_ = 0;
  bool sort_descending_ = false;
  bool filter_active_ = false;
  std::string filter_pattern_;

  bool LoadChunk(size_t chunk_index);
  void TouchChunk(size_t chunk_index);
  void EvictIfNeeded();
  std::streampos ResolveOffset(size_t chunk_index);
  void EnsureOffsetsUpTo(size_t chunk_index);
  bool GetPhysicalRow(size_t index, std::vector<std::string> &out);
  size_t ToPhysical(size_t view_index) const;
  void SampleColumnMetadata();
  void ResetDerivedState();
  void RebuildOrder();
};
