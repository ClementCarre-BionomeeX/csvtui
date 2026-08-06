#include "csv_model.h"

#include "csv_parser.h"
#include "csv_system.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <numeric>
#include <sys/stat.h>

namespace {

bool IsDirectory(const std::string &path) {
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0)
    return false;
  return S_ISDIR(info.st_mode);
}

bool PathExists(const std::string &path) {
  struct stat info {};
  return ::stat(path.c_str(), &info) == 0;
}

long long SizeOf(const std::string &path) {
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0)
    return 0;
  return static_cast<long long>(info.st_size);
}

} // namespace

CSVModel::~CSVModel() { Close(); }

std::string CSVModel::Open(const std::string &path,
                           std::optional<char> delimiter,
                           std::optional<bool> has_header) {
  Close();

  if (!PathExists(path))
    return "no such file: " + path;
  if (IsDirectory(path))
    return path + " is a directory, not a CSV file";

  file_.open(path, std::ios::binary);
  if (!file_.is_open())
    return "cannot read " + path + ": " + std::strerror(errno);

  display_path_ = path;
  real_path_ = path;
  file_size_ = SizeOf(path);

  std::string first_record;
  if (!csv::ReadRecord(file_, first_record)) {
    Close();
    return path + " is empty";
  }
  csv::StripBom(first_record);

  delimiter_ = delimiter.value_or(csv::DetectDelimiter(first_record));
  has_header_ = has_header.value_or(true);

  const std::vector<std::string> first_fields =
      csv::SplitRecord(first_record, delimiter_);

  if (has_header_) {
    header_ = first_fields;
    data_offset_ = file_.tellg();
  } else {
    header_.clear();
    for (size_t i = 0; i < first_fields.size(); ++i)
      header_.push_back("col" + std::to_string(i + 1));
    data_offset_ = 0; // the first record is data, so replay from the top
  }

  column_count_ = first_fields.size();
  chunk_offsets_.clear();
  chunk_offsets_.push_back(data_offset_);

  SampleColumnMetadata();
  RefineAverageRecordBytes();
  return std::string();
}

void CSVModel::RefineAverageRecordBytes() {
  if (total_rows_known_ || file_size_ <= 0 || !file_.is_open())
    return;

  const long long start = static_cast<long long>(data_offset_);
  const long long span = file_size_ - start;
  if (span <= 0)
    return;

  // The head of a file is often unrepresentative — identifiers are shorter,
  // optional columns are empty — so probe a few points further in.
  const double probes[] = {0.25, 0.5, 0.75};
  double sampled_bytes = 0.0;
  size_t sampled_records = 0;

  for (double fraction : probes) {
    const long long at = start + static_cast<long long>(span * fraction);
    file_.clear();
    file_.seekg(at);
    if (!file_)
      continue;

    std::string discard;
    if (!std::getline(file_, discard)) // resync onto a record boundary
      continue;

    const std::streampos begin = file_.tellg();
    if (begin == std::streampos(-1))
      continue;

    std::string record;
    size_t records = 0;
    while (records < 100 && csv::ReadRecord(file_, record))
      ++records;

    const std::streampos end = file_.tellg();
    if (records == 0 || end == std::streampos(-1) || end <= begin)
      continue;
    sampled_bytes += static_cast<double>(end) - static_cast<double>(begin);
    sampled_records += records;
  }

  if (sampled_records == 0)
    return;

  const double probed = sampled_bytes / static_cast<double>(sampled_records);
  average_record_bytes_ = average_record_bytes_ > 0.0
                              ? (average_record_bytes_ + probed * 3.0) / 4.0
                              : probed;
}

void CSVModel::Close() {
  if (file_.is_open())
    file_.close();
  ResetDerivedState();
  header_.clear();
  display_path_.clear();
  column_count_ = 0;
  chunk_offsets_.clear();
  data_offset_ = std::streampos(0);
}

void CSVModel::ResetDerivedState() {
  chunk_cache_.clear();
  lru_.clear();
  lru_pos_.clear();
  total_rows_ = 0;
  total_rows_known_ = false;
  file_size_ = 0;
  average_record_bytes_ = 0.0;
  column_widths_.clear();
  column_numeric_.clear();
  order_.clear();
  sort_active_ = false;
  filter_active_ = false;
  filter_pattern_.clear();
}

void CSVModel::SetHasHeader(bool value) {
  if (value == has_header_ || !file_.is_open())
    return;

  const std::string path = display_path_;
  const char delim = delimiter_;
  Open(path, delim, value);
}

// --- physical row access ----------------------------------------------------

void CSVModel::TouchChunk(size_t chunk_index) {
  auto it = lru_pos_.find(chunk_index);
  if (it != lru_pos_.end())
    lru_.erase(it->second);
  lru_.push_front(chunk_index);
  lru_pos_[chunk_index] = lru_.begin();
}

void CSVModel::EvictIfNeeded() {
  while (lru_.size() > kMaxCachedChunks) {
    const size_t victim = lru_.back();
    lru_.pop_back();
    lru_pos_.erase(victim);
    chunk_cache_.erase(victim);
  }
}

std::streampos CSVModel::ResolveOffset(size_t chunk_index) {
  if (chunk_index < chunk_offsets_.size())
    return chunk_offsets_[chunk_index];
  EnsureOffsetsUpTo(chunk_index);
  if (chunk_index < chunk_offsets_.size())
    return chunk_offsets_[chunk_index];
  return std::streampos(-1);
}

void CSVModel::EnsureOffsetsUpTo(size_t target_chunk) {
  if (!file_.is_open() || chunk_offsets_.empty())
    return;

  size_t current_chunk = chunk_offsets_.size() - 1;
  if (current_chunk >= target_chunk)
    return;

  file_.clear();
  file_.seekg(chunk_offsets_.back());
  if (!file_)
    return;

  size_t rows_seen = current_chunk * kChunkSize;
  std::string record;

  while (current_chunk < target_chunk) {
    size_t records = 0;
    for (; records < kChunkSize && csv::ReadRecord(file_, record); ++records)
      ++rows_seen;

    if (records < kChunkSize) {
      // Hit EOF: the file has exactly `rows_seen` rows.
      total_rows_ = rows_seen;
      total_rows_known_ = true;
      return;
    }

    ++current_chunk;
    const std::streampos offset = file_.tellg();
    if (offset == std::streampos(-1))
      return;
    chunk_offsets_.push_back(offset);
  }
}

bool CSVModel::LoadChunk(size_t chunk_index) {
  if (chunk_cache_.count(chunk_index)) {
    TouchChunk(chunk_index);
    return true;
  }

  const std::streampos offset = ResolveOffset(chunk_index);
  if (offset == std::streampos(-1))
    return false;

  file_.clear();
  file_.seekg(offset);
  if (!file_)
    return false;

  std::vector<std::vector<std::string>> rows;
  rows.reserve(kChunkSize);

  std::string record;
  size_t row_number = chunk_index * kChunkSize;
  for (size_t i = 0; i < kChunkSize; ++i) {
    if (!csv::ReadRecord(file_, record)) {
      total_rows_ = row_number;
      total_rows_known_ = true;
      break;
    }
    if (row_number == 0)
      csv::StripBom(record);
    rows.push_back(csv::SplitRecord(record, delimiter_));
    ++row_number;
  }

  if (rows.empty() && chunk_index > 0)
    return false;

  const std::streampos next_offset = file_.tellg();
  if (next_offset != std::streampos(-1) &&
      chunk_offsets_.size() == chunk_index + 1)
    chunk_offsets_.push_back(next_offset);

  for (const auto &row : rows)
    column_count_ = std::max(column_count_, row.size());

  chunk_cache_[chunk_index] = std::move(rows);
  TouchChunk(chunk_index);
  EvictIfNeeded();
  return true;
}

bool CSVModel::GetPhysicalRow(size_t index, std::vector<std::string> &out) {
  if (!file_.is_open())
    return false;
  if (total_rows_known_ && index >= total_rows_)
    return false;

  const size_t chunk_index = index / kChunkSize;
  if (!LoadChunk(chunk_index))
    return false;

  auto it = chunk_cache_.find(chunk_index);
  if (it == chunk_cache_.end())
    return false;

  const size_t offset_in_chunk = index - chunk_index * kChunkSize;
  if (offset_in_chunk >= it->second.size())
    return false;

  out = it->second[offset_in_chunk];
  return true;
}

size_t CSVModel::ToPhysical(size_t view_index) const {
  if (order_.empty())
    return view_index;
  if (view_index >= order_.size())
    return static_cast<size_t>(-1);
  return order_[view_index];
}

bool CSVModel::GetRow(size_t view_index, std::vector<std::string> &out) {
  if (!order_.empty()) {
    if (view_index >= order_.size())
      return false;
    return GetPhysicalRow(order_[view_index], out);
  }
  return GetPhysicalRow(view_index, out);
}

std::vector<std::vector<std::string>> CSVModel::GetRows(size_t start,
                                                        size_t count) {
  std::vector<std::vector<std::string>> rows;
  rows.reserve(count);
  std::vector<std::string> row;
  for (size_t i = 0; i < count; ++i) {
    if (!GetRow(start + i, row))
      break;
    rows.push_back(row);
  }
  return rows;
}

// --- counting ---------------------------------------------------------------

size_t CSVModel::EnsureTotalRowCount() {
  if (total_rows_known_)
    return total_rows_;
  if (!file_.is_open())
    return 0;

  file_.clear();
  file_.seekg(chunk_offsets_.empty() ? data_offset_ : chunk_offsets_.back());
  if (!file_)
    return total_rows_;

  size_t rows = chunk_offsets_.empty() ? 0 : (chunk_offsets_.size() - 1) * kChunkSize;
  std::string record;
  while (csv::ReadRecord(file_, record))
    ++rows;

  total_rows_ = rows;
  total_rows_known_ = true;
  return total_rows_;
}

size_t CSVModel::TotalRowCount() { return EnsureTotalRowCount(); }

size_t CSVModel::RowCount() {
  if (!order_.empty() || filter_active_)
    return order_.size();
  return EnsureTotalRowCount();
}

bool CSVModel::RowCountKnown() const {
  return filter_active_ || !order_.empty() || total_rows_known_;
}

// --- column metadata --------------------------------------------------------

void CSVModel::SampleColumnMetadata() {
  column_widths_.assign(std::max<size_t>(column_count_, 1), 0);
  column_numeric_.assign(std::max<size_t>(column_count_, 1), true);

  for (size_t i = 0; i < header_.size() && i < column_widths_.size(); ++i)
    column_widths_[i] = csv::DisplayWidth(header_[i]);

  std::vector<std::string> row;
  size_t sampled = 0;
  std::vector<size_t> numeric_seen(column_widths_.size(), 0);

  size_t sampled_bytes = 0;
  for (size_t index = 0; index < kSampleRows; ++index) {
    if (!GetPhysicalRow(index, row))
      break;
    ++sampled;
    // Reconstructed length: fields, separators, and the line terminator. Good
    // enough to turn a file size into a row estimate without reading it.
    for (const auto &field : row)
      sampled_bytes += field.size();
    sampled_bytes += row.empty() ? 1 : row.size(); // separators + newline
    if (row.size() > column_widths_.size()) {
      column_widths_.resize(row.size(), 0);
      column_numeric_.resize(row.size(), true);
      numeric_seen.resize(row.size(), 0);
    }
    for (size_t col = 0; col < row.size(); ++col) {
      const int width = csv::DisplayWidth(csv::SanitizeForDisplay(row[col]));
      column_widths_[col] = std::max(column_widths_[col], width);
      if (row[col].empty())
        continue;
      if (csv::IsNumeric(row[col]))
        ++numeric_seen[col];
      else
        column_numeric_[col] = false;
    }
  }

  for (size_t col = 0; col < column_widths_.size(); ++col) {
    column_widths_[col] = std::min(column_widths_[col], kMaxSampledWidth);
    column_widths_[col] = std::max(column_widths_[col], 1);
    if (numeric_seen[col] == 0)
      column_numeric_[col] = false;
  }
  column_count_ = std::max(column_count_, column_widths_.size());

  // Prefer real byte offsets over reconstructed field lengths: they account
  // for quoting and line endings exactly. Chunk boundaries give us the byte
  // position after a known number of rows for free.
  average_record_bytes_ = 0.0;
  if (chunk_offsets_.size() >= 2) {
    const size_t boundary = chunk_offsets_.size() - 1;
    const long long bytes = static_cast<long long>(chunk_offsets_[boundary]) -
                            static_cast<long long>(data_offset_);
    const size_t rows = boundary * kChunkSize;
    if (bytes > 0 && rows > 0)
      average_record_bytes_ = static_cast<double>(bytes) / static_cast<double>(rows);
  }
  if (average_record_bytes_ <= 0.0 && sampled > 0) {
    average_record_bytes_ =
        static_cast<double>(sampled_bytes) / static_cast<double>(sampled);
  }
}

// --- estimates --------------------------------------------------------------

size_t CSVModel::EstimatedRowCount() const {
  if (total_rows_known_)
    return total_rows_;
  if (average_record_bytes_ <= 0.0 || file_size_ <= 0)
    return 0;

  const long long payload =
      file_size_ - static_cast<long long>(data_offset_);
  if (payload <= 0)
    return 0;
  return static_cast<size_t>(static_cast<double>(payload) / average_record_bytes_);
}

double CSVModel::PositionFraction(size_t view_index) const {
  // With a sort or filter in play the byte position is meaningless, so fall
  // back to the position within the view.
  if (!order_.empty())
    return order_.empty() ? 0.0
                          : static_cast<double>(view_index) /
                                static_cast<double>(std::max<size_t>(order_.size(), 1));

  if (file_size_ <= 0)
    return 0.0;
  if (total_rows_known_ && total_rows_ > 0)
    return static_cast<double>(view_index) / static_cast<double>(total_rows_);

  // Use the nearest chunk offset we have already resolved: it is an exact byte
  // position, so this is accurate wherever the user has actually been.
  const size_t chunk = view_index / kChunkSize;
  if (chunk < chunk_offsets_.size()) {
    const double offset = static_cast<double>(chunk_offsets_[chunk]);
    return std::min(1.0, offset / static_cast<double>(file_size_));
  }

  const size_t estimate = EstimatedRowCount();
  if (estimate == 0)
    return 0.0;
  return std::min(1.0, static_cast<double>(view_index) /
                           static_cast<double>(estimate));
}

size_t CSVModel::EstimatedSortBytes() const {
  return EstimatedRowCount() * kSortBytesPerRow;
}

size_t CSVModel::EstimatedFilterBytes() const {
  return EstimatedRowCount() * kFilterBytesPerRow;
}

namespace {

std::string RefusalMessage(const char *operation, size_t rows, size_t needed,
                           size_t budget, size_t available) {
  (void)available;
  return std::string(operation) + " ~" + csv::HumanCount(rows) + " rows needs ~" +
         csv::HumanBytes(needed) + ", only " + csv::HumanBytes(budget) +
         " usable — filter first";
}

} // namespace

std::string CSVModel::CheckSortFeasible(size_t available_bytes) const {
  if (available_bytes == 0)
    return std::string(); // unknown: do not stand in the user's way
  const size_t budget =
      static_cast<size_t>(static_cast<double>(available_bytes) * kMemoryBudgetShare);
  const size_t needed = EstimatedSortBytes();
  if (needed <= budget)
    return std::string();
  return RefusalMessage("sorting", EstimatedRowCount(), needed, budget,
                        available_bytes);
}

std::string CSVModel::CheckFilterFeasible(size_t available_bytes) const {
  if (available_bytes == 0)
    return std::string();
  const size_t budget =
      static_cast<size_t>(static_cast<double>(available_bytes) * kMemoryBudgetShare);
  const size_t needed = EstimatedFilterBytes();
  if (needed <= budget)
    return std::string();
  return RefusalMessage("filtering", EstimatedRowCount(), needed, budget,
                        available_bytes);
}

std::string CSVModel::CheckSortFeasible() const {
  return CheckSortFeasible(csv::AvailableMemoryBytes());
}

std::string CSVModel::CheckFilterFeasible() const {
  return CheckFilterFeasible(csv::AvailableMemoryBytes());
}

void CSVModel::AdoptIndex(std::vector<std::streampos> offsets,
                          size_t total_rows) {
  if (offsets.empty())
    return;
  chunk_offsets_ = std::move(offsets);
  total_rows_ = total_rows;
  total_rows_known_ = true;
}

bool CSVModel::ColumnIsNumeric(size_t col) const {
  return col < column_numeric_.size() && column_numeric_[col];
}

std::string CSVModel::ColumnName(size_t col) const {
  if (col < header_.size() && !header_[col].empty())
    return header_[col];
  return "col" + std::to_string(col + 1);
}

// --- search -----------------------------------------------------------------

std::optional<CSVModel::SearchHit>
CSVModel::FindNext(const std::string &pattern, size_t row, size_t col,
                   bool wrap) {
  if (!file_.is_open() || pattern.empty())
    return std::nullopt;

  const bool ci = csv::SmartCaseInsensitive(pattern);
  // Searching walks forward until a row read fails, so it never needs the row
  // count and therefore never triggers a full scan just to get started.
  const bool bounded = !order_.empty();
  const size_t bound = order_.size();

  std::vector<std::string> fields;

  for (size_t current = row;; ++current) {
    if (bounded && current >= bound)
      break;
    if (!GetRow(current, fields))
      break;
    for (size_t c = (current == row ? col + 1 : 0); c < fields.size(); ++c) {
      const size_t pos = csv::FindFrom(fields[c], pattern, 0, ci);
      if (pos != std::string::npos)
        return SearchHit{current, c, pos, pattern.size()};
    }
  }

  if (!wrap)
    return std::nullopt;

  for (size_t current = 0; current <= row; ++current) {
    if (!GetRow(current, fields))
      break;
    for (size_t c = 0; c < fields.size(); ++c) {
      if (current == row && c > col)
        break;
      const size_t pos = csv::FindFrom(fields[c], pattern, 0, ci);
      if (pos != std::string::npos)
        return SearchHit{current, c, pos, pattern.size()};
    }
  }
  return std::nullopt;
}

std::optional<CSVModel::SearchHit>
CSVModel::FindPrev(const std::string &pattern, size_t row, size_t col,
                   bool wrap) {
  if (!file_.is_open() || pattern.empty())
    return std::nullopt;

  const bool ci = csv::SmartCaseInsensitive(pattern);
  // Walking backwards only needs a bound when wrapping round to the end, so
  // the common case costs no scan.
  if (!order_.empty() && row >= order_.size())
    row = order_.empty() ? 0 : order_.size() - 1;

  std::vector<std::string> fields;

  for (size_t step = 0; step <= row; ++step) {
    const size_t current = row - step;
    if (!GetRow(current, fields))
      break;
    const size_t upper =
        (step == 0) ? std::min(col, fields.size()) : fields.size();
    for (size_t c = upper; c-- > 0;) {
      if (c >= fields.size())
        continue;
      const size_t pos = csv::FindFrom(fields[c], pattern, 0, ci);
      if (pos != std::string::npos)
        return SearchHit{current, c, pos, pattern.size()};
    }
    if (current == 0)
      break;
  }

  if (!wrap)
    return std::nullopt;

  // Wrapping backwards is the one search path that needs to know where the end
  // is; skip it rather than forcing a scan the user did not ask for.
  if (!RowCountKnown())
    return std::nullopt;

  const size_t total = RowCount();
  for (size_t current = total; current-- > row;) {
    if (!GetRow(current, fields))
      continue;
    for (size_t c = fields.size(); c-- > 0;) {
      if (current == row && c <= col)
        break;
      const size_t pos = csv::FindFrom(fields[c], pattern, 0, ci);
      if (pos != std::string::npos)
        return SearchHit{current, c, pos, pattern.size()};
    }
  }
  return std::nullopt;
}

// --- ordering ---------------------------------------------------------------

void CSVModel::RebuildOrder() {
  const size_t total = EnsureTotalRowCount();

  std::vector<size_t> candidates;
  std::vector<std::string> fields;

  if (filter_active_ && !filter_pattern_.empty()) {
    const bool ci = csv::SmartCaseInsensitive(filter_pattern_);
    for (size_t i = 0; i < total; ++i) {
      if (!GetPhysicalRow(i, fields))
        break;
      const bool hit = std::any_of(
          fields.begin(), fields.end(), [&](const std::string &value) {
            return csv::FindFrom(value, filter_pattern_, 0, ci) !=
                   std::string::npos;
          });
      if (hit)
        candidates.push_back(i);
    }
  } else {
    candidates.resize(total);
    std::iota(candidates.begin(), candidates.end(), size_t{0});
  }

  if (sort_active_) {
    // Pre-extract the sort keys so comparison does not re-read the file.
    const size_t col = sort_column_;
    std::vector<std::pair<std::string, double>> keys(candidates.size());
    std::vector<char> is_number(candidates.size(), 0);
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (!GetPhysicalRow(candidates[i], fields))
        continue;
      const std::string value = col < fields.size() ? fields[col] : std::string();
      double number = 0.0;
      is_number[i] = csv::ParseNumber(value, number) ? 1 : 0;
      keys[i] = {value, number};
    }

    std::vector<size_t> slots(candidates.size());
    std::iota(slots.begin(), slots.end(), size_t{0});
    std::stable_sort(slots.begin(), slots.end(), [&](size_t a, size_t b) {
      if (is_number[a] && is_number[b])
        return keys[a].second < keys[b].second;
      if (is_number[a] != is_number[b])
        return is_number[a] > is_number[b]; // numbers before text
      return keys[a].first < keys[b].first;
    });
    if (sort_descending_)
      std::reverse(slots.begin(), slots.end());

    std::vector<size_t> ordered;
    ordered.reserve(slots.size());
    for (size_t slot : slots)
      ordered.push_back(candidates[slot]);
    candidates.swap(ordered);
  }

  const bool identity = !sort_active_ && !filter_active_;
  order_ = identity ? std::vector<size_t>() : std::move(candidates);
}

void CSVModel::SortByColumn(size_t col, bool descending) {
  sort_active_ = true;
  sort_column_ = col;
  sort_descending_ = descending;
  RebuildOrder();
}

void CSVModel::ClearSort() {
  sort_active_ = false;
  RebuildOrder();
}

size_t CSVModel::ApplyFilter(const std::string &pattern) {
  filter_pattern_ = pattern;
  filter_active_ = !pattern.empty();
  RebuildOrder();
  return filter_active_ ? order_.size() : EnsureTotalRowCount();
}

void CSVModel::ClearFilter() {
  filter_active_ = false;
  filter_pattern_.clear();
  RebuildOrder();
}

CSVModel::ColumnStats CSVModel::ComputeColumnStats(size_t col) {
  ColumnStats stats;
  const size_t total = RowCount();
  std::vector<std::string> fields;
  double sum = 0.0;
  bool first = true;

  for (size_t i = 0; i < total; ++i) {
    if (!GetRow(i, fields))
      break;
    ++stats.total;
    const std::string value = col < fields.size() ? fields[col] : std::string();
    if (value.empty()) {
      ++stats.empty;
      continue;
    }
    double number = 0.0;
    if (!csv::ParseNumber(value, number))
      continue;
    ++stats.numeric;
    sum += number;
    if (first) {
      stats.min = stats.max = number;
      first = false;
    } else {
      stats.min = std::min(stats.min, number);
      stats.max = std::max(stats.max, number);
    }
  }

  if (stats.numeric > 0)
    stats.mean = sum / static_cast<double>(stats.numeric);
  return stats;
}
