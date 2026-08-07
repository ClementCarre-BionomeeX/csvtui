#pragma once

#include <ftxui/dom/elements.hpp>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <vector>

class CSVModel;

// Renders the grid. The view owns presentation state only; the controller owns
// the cursor and tells the view where it is.
class CSVView {
public:
  explicit CSVView(CSVModel &model);

  // `width` and `height` are the full terminal dimensions.
  ftxui::Element Render(int width, int height);

  // How many data rows fit given the current chrome (header, status, ...).
  int RowsThatFit(int height) const;

  void ToggleHeaderPinned();
  void ToggleTabularMode();
  void ToggleHelp();
  void ToggleCellDetail();
  void CloseOverlays();
  bool HeaderPinned() const { return header_pinned_; }
  bool TabularMode() const { return tabular_mode_; }
  bool HelpVisible() const { return show_help_; }
  bool CellDetailVisible() const { return show_cell_detail_; }
  bool AnyOverlayVisible() const { return show_help_ || show_cell_detail_; }

  void SetCursor(size_t row, size_t col);
  void SetViewportStart(size_t row);
  void SetSearch(const std::string &pattern);
  void SetCurrentMatch(std::optional<size_t> row, std::optional<size_t> col);
  void SetCommandLine(const std::string &line);
  void SetStatusMessage(const std::string &message, bool is_error);

  // Widths are sampled from the first thousand rows so the table stops
  // reflowing as you scroll. That is the right trade until a long value first
  // appears at row fifty thousand and is clipped with no way to see it, which
  // is what these are for. Overrides survive until cleared.
  void WidenColumn(size_t col, int by);
  void FitColumnToScreen(size_t col, int height);
  void ClearColumnWidth(size_t col);
  bool ColumnWidthOverridden(size_t col) const;
  int ColumnWidth(size_t col) const;

  void ToggleColumnHidden(size_t col);
  void ShowAllColumns();
  bool ColumnHidden(size_t col) const;
  void SetFrozenColumns(size_t count);
  size_t FrozenColumns() const { return frozen_columns_; }

  // Scrolls horizontally so `col` is on screen; returns the first shown column.
  void EnsureColumnVisible(size_t col, int width);
  size_t FirstVisibleColumn() const { return first_column_; }

private:
  struct Layout {
    std::vector<size_t> columns; // column indices, in render order
    std::vector<int> widths;
    bool truncated_left = false;
    bool truncated_right = false;
  };

  CSVModel &model_;

  bool header_pinned_ = true;
  bool tabular_mode_ = true;
  bool show_help_ = false;
  bool show_cell_detail_ = false;

  size_t cursor_row_ = 0;
  size_t cursor_col_ = 0;
  size_t viewport_start_ = 0;
  size_t first_column_ = 0;
  size_t frozen_columns_ = 0;
  std::set<size_t> hidden_columns_;
  std::map<size_t, int> column_width_overrides_;

  std::string search_pattern_;
  bool search_ignore_case_ = false;
  std::optional<size_t> match_row_;
  std::optional<size_t> match_col_;

  std::string command_line_;
  std::string status_message_;
  bool status_is_error_ = false;

  // Filled in by Render() so the status bar can flag off-screen columns.
  bool last_truncated_left_ = false;
  bool last_truncated_right_ = false;

  Layout ComputeLayout(int available_width) const;
  ftxui::Element RenderCell(const std::string &raw, size_t col, int width,
                            bool is_header, bool cursor_cell,
                            std::optional<size_t> row_index) const;
  ftxui::Element RenderStatusBar(int width);
  ftxui::Element RenderHelp() const;
  ftxui::Element RenderCellDetail() const;
};
