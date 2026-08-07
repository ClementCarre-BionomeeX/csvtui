#pragma once

#include <fstream>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "csv_scan.h"

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

  // Computed by the same pass whether it runs here or on a worker thread.
  using ColumnStats = csvscan::Stats;

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
  // Name shown to the user, which is not always the file we read from.
  const std::string &path() const { return display_path_; }
  // The path actually on disk — always use this to open the file again.
  const std::string &real_path() const { return real_path_; }
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

  // Lets a search running on a worker thread say how far it has got and be
  // abandoned part way. Both may be empty, in which case the search behaves
  // exactly as it always did.
  //
  // A search that walks a whole file takes as long as counting one does, so it
  // belongs off the UI thread. Nothing here makes CSVModel thread-safe: the
  // caller must guarantee that no other thread touches the model meanwhile,
  // which the controller does by refusing every key but Esc while it runs.
  struct SearchWatch {
    std::function<bool()> cancelled;
    std::function<void(size_t rows_examined)> report;
  };

  std::optional<SearchHit> FindNext(const std::string &pattern, size_t row,
                                    size_t col, bool wrap,
                                    const SearchWatch &watch = {});
  std::optional<SearchHit> FindPrev(const std::string &pattern, size_t row,
                                    size_t col, bool wrap,
                                    const SearchWatch &watch = {});

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

  // --- size estimates, none of which read the file -------------------------

  long long FileSize() const { return file_size_; }
  std::streampos DataOffset() const { return data_offset_; }
  // Rows implied by the file size and the average record length seen while
  // sampling. Exact once the file has been counted.
  size_t EstimatedRowCount() const;
  bool RowCountIsExact() const { return total_rows_known_; }

  // Roughly how far into the file a view row sits, as 0..1. Derived from the
  // nearest known chunk offset, so it costs nothing.
  double PositionFraction(size_t view_index) const;

  // Peak extra memory these operations would need, from the row estimate.
  size_t EstimatedSortBytes() const;
  size_t EstimatedFilterBytes() const;

  // How much a sort may spend on keys before spilling a run to disk. What is
  // left of the budget once the resulting order is accounted for, bounded at
  // both ends.
  size_t SortMemoryBudget(size_t available_bytes) const;
  size_t SortMemoryBudget() const;

  // Empty when the operation fits in `available_bytes`, otherwise a sentence
  // explaining why it was refused. `available_bytes == 0` means "unknown",
  // which never refuses.
  std::string CheckSortFeasible(size_t available_bytes) const;
  std::string CheckFilterFeasible(size_t available_bytes) const;
  std::string CheckSortFeasible() const;
  std::string CheckFilterFeasible() const;

  // Replaces the chunk offset table and row count with the results of a scan
  // performed elsewhere (see CSVScanner).
  void AdoptIndex(std::vector<std::streampos> offsets, size_t total_rows);

  // The offset table is expensive to build and small to keep, so it outlives
  // the session. LoadIndex is tried on open; SaveIndex is worth calling
  // whenever a full pass has just made the count exact. Both are best-effort:
  // a cache that cannot be read or written costs only the time it saved.
  bool LoadIndex();
  bool SaveIndex() const;
  // True when the row count came from a cache rather than from reading.
  bool row_count_came_from_cache() const { return count_from_cache_; }

  // The sort and filter currently in effect. Handed to CSVScanner to describe
  // the view a background pass should produce, and back to AdoptView with the
  // index it built.
  struct ViewState {
    bool sort_active = false;
    size_t sort_column = 0;
    bool sort_descending = false;
    bool filter_active = false;
    std::string filter_pattern;
  };

  ViewState CurrentViewState() const;
  // Installs an ordering computed elsewhere. `has_order` false means the view
  // is the file in its own order, which is stored as no index at all.
  void AdoptView(const ViewState &state, std::vector<size_t> order,
                 bool has_order);
  // Fills a scan request describing this model, so callers do not have to know
  // which of its internals the scanner needs.
  void DescribeScan(csvscan::Request &request) const;

  static constexpr size_t kChunkSize = 512;
  // A sort holds one key per row while it works — measured at ~61 bytes — but
  // those spill to temporary files once the buffer is full, so the only cost
  // that follows the row count is the answer: one row number per row.
  static constexpr size_t kSortKeyBytesPerRow = 61;
  static constexpr size_t kSortOutputBytesPerRow = sizeof(size_t);
  static constexpr size_t kFilterBytesPerRow = 10;
  // How much a sort may hold before spilling. The floor is what makes an
  // enormous sort possible at all; the ceiling stops a roomy machine from
  // buffering far more than merging a few extra runs would have cost.
  static constexpr size_t kMinSortBufferBytes = 32u * 1024 * 1024;
  static constexpr size_t kMaxSortBufferBytes = 512u * 1024 * 1024;
  // Never spend more than this share of what is available.
  static constexpr double kMemoryBudgetShare = 0.6;

private:
  static constexpr size_t kMaxCachedChunks = 48; // ~24k rows resident
  static constexpr size_t kSampleRows = 1000;
  static constexpr int kMaxSampledWidth = 48;

  std::ifstream file_;
  std::string display_path_;
  std::string real_path_;
  std::streampos data_offset_{0};
  char delimiter_ = ',';
  bool has_header_ = true;
  std::vector<std::string> header_;
  size_t column_count_ = 0;

  size_t total_rows_ = 0;
  bool total_rows_known_ = false;
  bool count_from_cache_ = false;
  long long file_size_ = 0;
  double average_record_bytes_ = 0.0;

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
  // Samples record lengths at a few points in the file so the row estimate is
  // not skewed by an unrepresentative head.
  void RefineAverageRecordBytes();
};
