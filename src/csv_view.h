
#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/screen/terminal.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class CSVModel; // Forward declaration

class CSVView {
public:
  CSVView(CSVModel &model);

  ftxui::Component Render();
  void ToggleHeaderPinned();
  void ToggleTabularMode();
  void AdjustColumnOffset(int delta);
  void SetCommandLine(const std::string &current, const std::string &last);
  void SetSearchPattern(const std::string &pattern);
  void SetStartRow(size_t start_row);
  void SetCurrentMatch(std::optional<size_t> row, std::optional<size_t> col);
  bool IsHeaderPinned() const { return header_pinned_; }

private:
  CSVModel &model_;
  bool header_pinned_ = true;
  bool tabular_mode_ = true;
  size_t column_offset_ = 0;
  std::string current_command_;
  std::string last_command_;
  std::string search_pattern_;
  size_t start_row_base_ = 0;
  std::optional<size_t> current_match_row_;
  std::optional<size_t> current_match_col_;

  std::vector<ftxui::Color> column_colors_;
  void InitColumnColors(size_t count);
  size_t MaxColumns(const std::vector<std::vector<std::string>> &rows) const;

  std::vector<std::vector<std::string>> GetVisibleRows();
  std::vector<size_t>
  ComputeColumnWidths(const std::vector<std::vector<std::string>> &rows);

  ftxui::Element FormatRow(const std::vector<std::string> &row,
                           const std::vector<size_t> &column_widths,
                           bool is_header,
                           std::optional<size_t> row_index,
                           size_t start_col,
                           int available_width);
};
