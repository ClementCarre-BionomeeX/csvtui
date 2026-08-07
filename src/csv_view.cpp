#include "csv_view.h"

#include "csv_model.h"
#include "csv_parser.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

using namespace ftxui;

namespace {

constexpr int kMinColumnWidth = 3;
constexpr int kMaxColumnWidth = 40;
constexpr int kGutterWidth = 1; // the separator between two columns

std::string FormatCount(size_t value) {
  std::string digits = std::to_string(value);
  std::string out;
  int since = 0;
  for (size_t i = digits.size(); i-- > 0;) {
    out.push_back(digits[i]);
    if (++since == 3 && i > 0) {
      out.push_back(' ');
      since = 0;
    }
  }
  std::reverse(out.begin(), out.end());
  return out;
}

} // namespace

CSVView::CSVView(CSVModel &model) : model_(model) {}

// --- presentation state -----------------------------------------------------

void CSVView::ToggleHeaderPinned() { header_pinned_ = !header_pinned_; }
void CSVView::ToggleTabularMode() { tabular_mode_ = !tabular_mode_; }

void CSVView::ToggleHelp() {
  show_help_ = !show_help_;
  if (show_help_)
    show_cell_detail_ = false;
}

void CSVView::ToggleCellDetail() {
  show_cell_detail_ = !show_cell_detail_;
  if (show_cell_detail_)
    show_help_ = false;
}

void CSVView::CloseOverlays() {
  show_help_ = false;
  show_cell_detail_ = false;
}

void CSVView::SetCursor(size_t row, size_t col) {
  cursor_row_ = row;
  cursor_col_ = col;
}

void CSVView::SetViewportStart(size_t row) { viewport_start_ = row; }

void CSVView::SetSearch(const std::string &pattern) {
  search_pattern_ = pattern;
  search_ignore_case_ = csv::SmartCaseInsensitive(pattern);
}

void CSVView::SetCurrentMatch(std::optional<size_t> row,
                              std::optional<size_t> col) {
  match_row_ = row;
  match_col_ = col;
}

void CSVView::SetCommandLine(const std::string &line) { command_line_ = line; }

void CSVView::SetStatusMessage(const std::string &message, bool is_error) {
  status_message_ = message;
  status_is_error_ = is_error;
}

void CSVView::ToggleColumnHidden(size_t col) {
  if (!hidden_columns_.insert(col).second)
    hidden_columns_.erase(col);
}

void CSVView::ShowAllColumns() {
  hidden_columns_.clear();
  column_width_overrides_.clear();
}

bool CSVView::ColumnHidden(size_t col) const {
  return hidden_columns_.count(col) > 0;
}

void CSVView::SetFrozenColumns(size_t count) { frozen_columns_ = count; }

int CSVView::RowsThatFit(int height) const {
  int rows = height;
  rows -= 2; // status bar + message line, both always reserved so that the
             // viewport height does not jump when a message appears
  if (header_pinned_ && !model_.Header().empty())
    rows -= 2; // header + rule
  return std::max(rows, 1);
}

int CSVView::ColumnWidth(size_t col) const {
  const auto override_it = column_width_overrides_.find(col);
  if (override_it != column_width_overrides_.end())
    return override_it->second;

  const auto &widths = model_.ColumnWidths();
  int width = col < widths.size() ? widths[col] : kMinColumnWidth;
  width = std::clamp(width, kMinColumnWidth, kMaxColumnWidth);
  return width;
}

void CSVView::WidenColumn(size_t col, int by) {
  // The ceiling is deliberately higher than the sampled one: the whole point
  // is to see a value the sampling decided was too long to show.
  constexpr int kWidestByHand = 400;
  const int width = std::clamp(ColumnWidth(col) + by, kMinColumnWidth,
                               kWidestByHand);
  column_width_overrides_[col] = width;
}

void CSVView::FitColumnToScreen(size_t col, int height) {
  // What is actually on screen, header included — not what the first thousand
  // rows happened to contain.
  int widest = 0;
  if (model_.has_header()) {
    const std::vector<std::string> &header = model_.Header();
    if (col < header.size())
      widest = csv::DisplayWidth(csv::SanitizeForDisplay(header[col]));
  }

  const int rows = std::max(1, RowsThatFit(height));
  std::vector<std::string> fields;
  for (int i = 0; i < rows; ++i) {
    if (!model_.GetRow(viewport_start_ + static_cast<size_t>(i), fields))
      break;
    if (col < fields.size()) {
      widest = std::max(widest,
                        csv::DisplayWidth(csv::SanitizeForDisplay(fields[col])));
    }
  }

  column_width_overrides_[col] = std::max(widest, kMinColumnWidth);
}

void CSVView::ClearColumnWidth(size_t col) { column_width_overrides_.erase(col); }

bool CSVView::ColumnWidthOverridden(size_t col) const {
  return column_width_overrides_.count(col) != 0;
}

// --- layout -----------------------------------------------------------------

CSVView::Layout CSVView::ComputeLayout(int available_width) const {
  Layout layout;
  const size_t total_columns = std::max<size_t>(model_.ColumnCount(), 1);

  int remaining = available_width;
  size_t frozen_end = std::min(frozen_columns_, total_columns);

  for (size_t col = 0; col < frozen_end; ++col) {
    if (ColumnHidden(col))
      continue;
    const int width = ColumnWidth(col);
    if (remaining < width + kGutterWidth && !layout.columns.empty())
      break;
    layout.columns.push_back(col);
    layout.widths.push_back(width);
    remaining -= width + kGutterWidth;
  }

  size_t start = std::max(first_column_, frozen_end);
  layout.truncated_left = start > frozen_end;

  for (size_t col = start; col < total_columns; ++col) {
    if (ColumnHidden(col))
      continue;
    const int width = ColumnWidth(col);
    if (remaining < width + kGutterWidth && !layout.columns.empty()) {
      layout.truncated_right = true;
      break;
    }
    layout.columns.push_back(col);
    layout.widths.push_back(width);
    remaining -= width + kGutterWidth;
  }

  if (layout.columns.empty()) {
    layout.columns.push_back(std::min(first_column_, total_columns - 1));
    layout.widths.push_back(std::max(available_width - kGutterWidth, 1));
  }
  return layout;
}

void CSVView::EnsureColumnVisible(size_t col, int width) {
  const size_t frozen_end = frozen_columns_;
  if (col < frozen_end)
    return;
  if (col < first_column_) {
    first_column_ = col;
    return;
  }

  // Walk backwards from `col` until the budget is spent; that is the leftmost
  // column that still leaves `col` on screen.
  int budget = width;
  for (size_t f = 0; f < std::min(frozen_end, model_.ColumnCount()); ++f) {
    if (!ColumnHidden(f))
      budget -= ColumnWidth(f) + kGutterWidth;
  }

  size_t leftmost = col;
  int used = 0;
  for (size_t c = col + 1; c-- > frozen_end;) {
    if (ColumnHidden(c))
      continue;
    const int needed = ColumnWidth(c) + kGutterWidth;
    if (used + needed > budget)
      break;
    used += needed;
    leftmost = c;
    if (c == frozen_end)
      break;
  }
  if (leftmost > first_column_)
    first_column_ = leftmost;
}

// --- cells ------------------------------------------------------------------

Element CSVView::RenderCell(const std::string &raw, size_t col, int width,
                            bool is_header, bool cursor_cell,
                            std::optional<size_t> row_index) const {
  const std::string clean = csv::SanitizeForDisplay(raw);
  const std::string shown =
      tabular_mode_ ? csv::TruncateToWidth(clean, width) : clean;
  const int shown_width = csv::DisplayWidth(shown);
  const int pad = tabular_mode_ ? std::max(0, width - shown_width) : 0;
  const bool right_align =
      tabular_mode_ && !is_header && model_.ColumnIsNumeric(col);

  std::vector<Element> segments;
  if (right_align && pad > 0)
    segments.push_back(text(std::string(pad, ' ')));

  const bool is_current_match = row_index && match_row_ && match_col_ &&
                                *row_index == *match_row_ && col == *match_col_;

  if (!search_pattern_.empty() && !is_header) {
    size_t at = 0;
    size_t cursor = 0;
    bool first_match = true;
    while ((at = csv::FindFrom(shown, search_pattern_, cursor,
                               search_ignore_case_)) != std::string::npos) {
      if (at > cursor)
        segments.push_back(text(shown.substr(cursor, at - cursor)));
      const std::string hit = shown.substr(at, search_pattern_.size());
      Element marked = text(hit);
      if (is_current_match && first_match)
        marked = marked | bgcolor(Color::Orange1) | color(Color::Black) | bold;
      else
        marked = marked | bgcolor(Color::Yellow) | color(Color::Black);
      segments.push_back(marked);
      cursor = at + search_pattern_.size();
      first_match = false;
      if (search_pattern_.empty())
        break;
    }
    if (cursor < shown.size())
      segments.push_back(text(shown.substr(cursor)));
  } else {
    segments.push_back(text(shown));
  }

  if (!right_align && pad > 0)
    segments.push_back(text(std::string(pad, ' ')));

  Element cell = hbox(std::move(segments));
  if (is_header)
    cell = cell | bold | color(Color::Cyan);
  if (cursor_cell)
    cell = cell | inverted;
  return cell;
}

// --- status -----------------------------------------------------------------

Element CSVView::RenderStatusBar(int width) {
  // Importance, lowest first: position (1), an active filter (2), the cursor
  // column (3), the file name (4), an active sort (5), the delimiter (6).
  // Segments are added most-important first and dropped once the bar is full,
  // so a narrow terminal degrades to the position indicator rather than to a
  // row of half-cut labels.
  struct Segment {
    std::string label;
    Color tint = Color::White;
    bool tinted = false;
    int importance = 5; // lower is kept longer when space runs out
  };
  std::vector<Segment> segments;

  // The percentage comes from the byte position, so it is available instantly
  // even on a file whose rows have never been counted.
  const bool known = model_.RowCountKnown();
  const int percent =
      static_cast<int>(std::lround(100.0 * model_.PositionFraction(cursor_row_)));

  std::string position = " row " + FormatCount(cursor_row_ + 1);
  if (known) {
    position += "/" + FormatCount(model_.RowCount());
  } else {
    // "~" is doing real work here: it is an estimate from the file size, not a
    // number anybody counted.
    const size_t estimate = model_.EstimatedRowCount();
    position += estimate > 0 ? "/~" + FormatCount(estimate) : "/…";
  }
  position += " (" + std::to_string(percent) + "%)";
  segments.push_back({position + " ", Color::White, false, 1});

  const size_t columns = std::max<size_t>(model_.ColumnCount(), 1);
  // The arrows say "there are more columns that way", which is otherwise
  // invisible once the view has scrolled sideways.
  const std::string more_left = last_truncated_left_ ? "‹" : " ";
  const std::string more_right = last_truncated_right_ ? "›" : " ";
  segments.push_back({" " + more_left + "col " + std::to_string(cursor_col_ + 1) +
                          "/" + std::to_string(columns) + " " +
                          model_.ColumnName(cursor_col_) + more_right + " ",
                      Color::White, false, 3});

  std::string name = model_.path();
  const size_t slash = name.find_last_of('/');
  if (slash != std::string::npos)
    name = name.substr(slash + 1);
  segments.insert(segments.begin(), {" " + name + " ", Color::White, false, 4});

  if (model_.sort_active()) {
    segments.push_back({std::string(" sort ") +
                            (model_.sort_descending() ? "↓ " : "↑ ") +
                            model_.ColumnName(model_.sort_column()) + " ",
                        Color::GreenLight, true, 5});
  }
  if (model_.filter_active()) {
    // Ranked above the column indicator on purpose. A filter changes which
    // rows exist; losing it to make room left the row count as the only hint
    // that what is on screen is not the whole file.
    segments.push_back({" filter '" + model_.filter_pattern() + "' ",
                        Color::MagentaLight, true, 2});
  }

  std::string delim_label(1, model_.delimiter());
  if (model_.delimiter() == '\t')
    delim_label = "\\t";
  segments.push_back({" delim '" + delim_label + "' ", Color::White, false, 6});

  const std::string hint = " ? help  q quit ";
  int budget = std::max(width, 1);
  const bool room_for_hint = budget >= csv::DisplayWidth(hint) + 12;
  if (room_for_hint)
    budget -= csv::DisplayWidth(hint);

  // Decide what to keep in importance order, then render in display order.
  std::vector<size_t> ranked(segments.size());
  std::iota(ranked.begin(), ranked.end(), size_t{0});
  std::stable_sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b) {
    return segments[a].importance < segments[b].importance;
  });

  std::vector<bool> keep(segments.size(), false);
  bool any_kept = false;
  for (size_t index : ranked) {
    const int cost =
        csv::DisplayWidth(segments[index].label) + (any_kept ? 1 : 0);
    if (cost > budget)
      continue;
    budget -= cost;
    keep[index] = true;
    any_kept = true;
  }

  std::vector<Element> rendered;
  for (size_t index = 0; index < segments.size(); ++index) {
    if (!keep[index])
      continue;
    if (!rendered.empty())
      rendered.push_back(text("│") | dim);
    Element element = text(segments[index].label);
    if (segments[index].tinted)
      element = element | color(segments[index].tint);
    rendered.push_back(element);
  }

  std::vector<Element> bar{hbox(std::move(rendered)), filler()};
  if (room_for_hint)
    bar.push_back(text(hint) | dim);

  return hbox(std::move(bar)) | bgcolor(Color::GrayDark) | color(Color::White) |
         size(WIDTH, EQUAL, std::max(width, 1));
}

Element CSVView::RenderHelp() const {
  struct Row {
    const char *keys;
    const char *what;
  };
  // Two columns, because one does not fit. Twenty-four bindings in a
  // twenty-four row terminal silently lost the last three, which were Esc, q
  // and the mouse — that is, the overlay explaining the keys was hiding how to
  // quit. Descriptions are terse to earn the second column; the man page has
  // the long form.
  static const Row rows[] = {
      {"h j k l / ←↓↑→", "move cursor"},
      {"Ctrl-D / Ctrl-U", "half page"},
      {"Ctrl-F / Ctrl-B", "full page"},
      {"PgDn / PgUp", "full page"},
      {"gg / G", "first / last row"},
      {"<n>G", "go to row n"},
      {"0 / $", "first / last column"},
      {"/ then Enter", "search forward"},
      {"n / N", "next / prev match"},
      {"f then Enter", "filter rows"},
      {"w then Enter", "write view to a file"},
      {"s / S", "sort by column"},
      {"u", "clear sort / filter"},
      {"x / X", "hide / show columns"},
      {"z", "freeze to cursor"},
      {"< / >", "narrow / widen column"},
      {"=", "fit column to screen"},
      {"Enter", "show full cell"},
      {"y", "copy cell"},
      {"c", "column statistics"},
      {"H", "pin / unpin header"},
      {"t", "aligned / raw mode"},
      {"?", "toggle this help"},
      {"Esc", "close / cancel"},
      {"q", "quit"},
      {"mouse", "scroll and click"},
  };
  constexpr size_t kRowCount = sizeof(rows) / sizeof(rows[0]);

  const auto entry = [](const Row &row) {
    return hbox({
        text(row.keys) | color(Color::Cyan) | size(WIDTH, EQUAL, 17),
        text(row.what),
    });
  };

  const size_t half = (kRowCount + 1) / 2;
  std::vector<Element> left, right;
  for (size_t i = 0; i < kRowCount; ++i)
    (i < half ? left : right).push_back(entry(rows[i]));
  // Keep the columns the same length so the frame does not jump about.
  while (right.size() < left.size())
    right.push_back(text(""));

  Element body = hbox({
      vbox(std::move(left)) | size(WIDTH, EQUAL, 38),
      separator(),
      vbox(std::move(right)) | size(WIDTH, EQUAL, 38),
  });

  return window(text(" csvtui — keys ") | bold, std::move(body)) |
         bgcolor(Color::Black) | clear_under | center;
}


Element CSVView::RenderCellDetail() const {
  std::vector<std::string> fields;
  std::string value;
  if (model_.GetRow(cursor_row_, fields) && cursor_col_ < fields.size())
    value = fields[cursor_col_];

  const std::string title = " " + model_.ColumnName(cursor_col_) + " — row " +
                            std::to_string(cursor_row_ + 1) + " ";

  std::vector<Element> body;
  if (value.empty()) {
    body.push_back(text("(empty)") | dim);
  } else {
    std::istringstream stream(csv::SanitizeForDisplay(value));
    std::string line;
    while (std::getline(stream, line))
      body.push_back(paragraph(line));
  }
  body.push_back(text(""));
  body.push_back(text("Enter or Esc to close") | dim);

  return window(text(title) | bold, vbox(std::move(body))) |
         size(WIDTH, LESS_THAN, 80) | bgcolor(Color::Black) | clear_under |
         center;
}

// --- main render ------------------------------------------------------------

Element CSVView::Render(int width, int height) {
  const auto &header = model_.Header();
  const bool has_header = !header.empty();
  const int available_width = std::max(width, 8);

  const Layout layout = ComputeLayout(available_width);
  last_truncated_left_ = layout.truncated_left;
  last_truncated_right_ = layout.truncated_right;
  const int visible_rows = RowsThatFit(height);
  const auto rows = model_.GetRows(viewport_start_, static_cast<size_t>(visible_rows));

  auto build_row = [&](const std::vector<std::string> &values, bool is_header,
                       std::optional<size_t> row_index) {
    std::vector<Element> cells;
    for (size_t i = 0; i < layout.columns.size(); ++i) {
      const size_t col = layout.columns[i];
      const std::string value = col < values.size() ? values[col] : std::string();
      const bool cursor_cell =
          !is_header && row_index && *row_index == cursor_row_ && col == cursor_col_;
      cells.push_back(RenderCell(value, col, layout.widths[i], is_header,
                                 cursor_cell, row_index));
      if (i + 1 < layout.columns.size())
        cells.push_back(text("│") | dim);
    }
    Element line = hbox(std::move(cells));
    if (!is_header && row_index && *row_index == cursor_row_)
      line = line | bgcolor(Color::GrayDark);
    return line;
  };

  std::vector<Element> body;

  if (has_header && header_pinned_) {
    std::vector<Element> header_line;
    if (layout.truncated_left)
      header_line.push_back(text("‹") | color(Color::Yellow));
    header_line.push_back(build_row(header, true, std::nullopt) | flex);
    if (layout.truncated_right)
      header_line.push_back(text("›") | color(Color::Yellow));
    body.push_back(hbox(std::move(header_line)));
    body.push_back(separator() | dim);
  }

  for (size_t i = 0; i < rows.size(); ++i)
    body.push_back(build_row(rows[i], false, viewport_start_ + i));

  if (rows.empty()) {
    body.push_back(filler());
    body.push_back(text(model_.filter_active()
                            ? "No rows match the current filter (u to clear)"
                            : "No data") |
                   dim | center);
  }

  Element table = vbox(std::move(body)) | flex;

  if (show_help_)
    table = dbox({table, RenderHelp()});
  else if (show_cell_detail_)
    table = dbox({table, RenderCellDetail()});

  // Bottom line: whatever is being typed, otherwise the last message. Long
  // messages (column statistics, for instance) get a full row instead of being
  // squeezed out of the status bar.
  Element message_line = text("");
  if (!command_line_.empty())
    message_line = text(command_line_);
  else if (!status_message_.empty())
    message_line = text(status_message_) |
                   color(status_is_error_ ? Color::Red : Color::GreenLight);

  return vbox({table, RenderStatusBar(width), message_line});
}
