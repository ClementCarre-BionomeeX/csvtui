
#include "csv_view.h"
#include "csv_model.h" // You'll need to define this later
#include <algorithm>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

using namespace ftxui;

CSVView::CSVView(CSVModel &model) : model_(model) {
  InitColumnColors(16); // Start with 16 columns
}

void CSVView::InitColumnColors(size_t count) {
  std::vector<Color> palette = {
      Color::Red,          Color::Green,         Color::Blue,
      Color::Yellow,       Color::Magenta,       Color::Cyan,
      Color::GrayDark,     Color::White,         Color::Orange1,
      Color::Purple,       Color::Turquoise2,    Color::SpringGreen1,
      Color::DeepPink3,    Color::DarkGoldenrod, Color::DodgerBlue2,
      Color::LightSalmon1,
  };

  column_colors_.clear();
  for (size_t i = 0; i < count; ++i)
    column_colors_.push_back(palette[i % palette.size()]);
}

Component CSVView::Render() {
  std::vector<std::vector<std::string>> rows = GetVisibleRows();
  const auto header = model_.GetHeader();
  const bool has_header = !header.empty();

  if (rows.empty() && !has_header)
    return Renderer([] { return text("No data loaded"); });

  const int available_width =
      std::max(1, Terminal::Size().dimx - 2); // Account for border

  // Header is part of the viewport only when we are at the top of the file.
  const bool header_in_rows =
      has_header && !rows.empty() && rows.front() == header;
  if (header_pinned_ && header_in_rows)
    rows.erase(rows.begin());

  std::vector<std::vector<std::string>> width_rows = rows;
  if (has_header)
    width_rows.insert(width_rows.begin(), header);

  std::vector<size_t> column_widths =
      tabular_mode_ ? ComputeColumnWidths(width_rows) : std::vector<size_t>{};

  std::vector<Element> rendered_rows;

  if (header_pinned_ && has_header)
    rendered_rows.push_back(
        FormatRow(header, column_widths, true, std::nullopt, column_offset_,
                  available_width));

  for (size_t i = 0; i < rows.size(); ++i) {
    const bool is_header_row = header_in_rows && !header_pinned_ && i == 0;
    std::optional<size_t> row_index = std::nullopt;
    if (!is_header_row) {
      size_t data_idx = (header_in_rows && !header_pinned_) ? i - 1 : i;
      row_index = start_row_base_ + data_idx;
    }
    rendered_rows.push_back(FormatRow(rows[i], column_widths, is_header_row,
                                      row_index, column_offset_, available_width));
  }

  auto status_line =
      hbox({text(":" + current_command_),
            filler(),
            text("Last: " + last_command_)}) |
      bgcolor(Color::GrayDark);

  return Renderer([rendered_rows, status_line]() {
    auto table = vbox(rendered_rows) | border;
    return vbox({table | flex, status_line});
  });
}

void CSVView::ToggleHeaderPinned() { header_pinned_ = !header_pinned_; }

void CSVView::ToggleTabularMode() { tabular_mode_ = !tabular_mode_; }

void CSVView::AdjustColumnOffset(int delta) {
  auto rows = GetVisibleRows();
  size_t max_cols = MaxColumns(rows);
  int next = static_cast<int>(column_offset_) + delta;
  if (next < 0)
    next = 0;
  if (max_cols == 0)
    column_offset_ = static_cast<size_t>(next);
  else
    column_offset_ = std::min(static_cast<size_t>(next), max_cols - 1);
}

std::vector<std::vector<std::string>> CSVView::GetVisibleRows() {
  // Placeholder — the real implementation should request the model.
  return model_.GetVisibleRows();
}

std::vector<size_t> CSVView::ComputeColumnWidths(
    const std::vector<std::vector<std::string>> &rows) {
  size_t num_cols = 0;
  for (const auto &row : rows)
    num_cols = std::max(num_cols, row.size());

  std::vector<size_t> widths(num_cols, 0);
  for (const auto &row : rows) {
    for (size_t i = 0; i < row.size(); ++i) {
      widths[i] = std::max(widths[i], row[i].size());
    }
  }

  return widths;
}

size_t
CSVView::MaxColumns(const std::vector<std::vector<std::string>> &rows) const {
  size_t max_cols = model_.GetHeader().size();
  for (const auto &row : rows)
    max_cols = std::max(max_cols, row.size());
  return max_cols;
}

Element CSVView::FormatRow(const std::vector<std::string> &row,
                           const std::vector<size_t> &column_widths,
                           bool is_header,
                           std::optional<size_t> row_index,
                           size_t start_col,
                           int available_width) {
  std::vector<Element> cells;
  int used_width = 0;

  for (size_t i = start_col; i < row.size(); ++i) {
    std::string value = row[i];
    size_t width = (tabular_mode_ && i < column_widths.size())
                       ? column_widths[i]
                       : value.size();
    int cell_width = static_cast<int>(width + 1); // + ';'
    if (!cells.empty() && used_width + cell_width > available_width)
      break;
    used_width += cell_width;
    std::vector<Element> segments;
    if (!search_pattern_.empty()) {
      auto pos = value.find(search_pattern_);
      if (pos != std::string::npos) {
        std::string pre = value.substr(0, pos);
        std::string mid = value.substr(pos, search_pattern_.size());
        std::string post = value.substr(pos + search_pattern_.size());
        if (!pre.empty())
          segments.push_back(text(pre));
        bool is_current = row_index && current_match_row_ &&
                          current_match_col_ && *row_index == *current_match_row_ &&
                          i == *current_match_col_;
        auto hl = is_current ? bgcolor(Color::RedLight) : bgcolor(Color::Yellow);
        segments.push_back(text(mid) | hl);
        if (!post.empty())
          segments.push_back(text(post));
      } else {
        segments.push_back(text(value));
      }
    } else {
      segments.push_back(text(value));
    }
    int pad = static_cast<int>(width - value.size());
    if (pad > 0)
      segments.push_back(text(std::string(pad, ' ')));
    segments.push_back(text(";"));

    Element cell = hbox(std::move(segments)) |
                   color(column_colors_[i % column_colors_.size()]);
    if (is_header)
      cell |= bold;

    cells.push_back(cell);
  }

  return hbox(std::move(cells)) | underlined;
}

void CSVView::SetCommandLine(const std::string &current,
                             const std::string &last) {
  current_command_ = current;
  last_command_ = last;
}

void CSVView::SetSearchPattern(const std::string &pattern) {
  search_pattern_ = pattern;
}

void CSVView::SetStartRow(size_t start_row) { start_row_base_ = start_row; }

void CSVView::SetCurrentMatch(std::optional<size_t> row,
                              std::optional<size_t> col) {
  current_match_row_ = row;
  current_match_col_ = col;
}
