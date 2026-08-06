#include "csv_controller.h"

#include "csv_model.h"
#include "csv_parser.h"
#include "csv_view.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/terminal.hpp>

using namespace ftxui;

namespace {

// Guard against overflow when a user leans on a digit key.
constexpr size_t kMaxCount = 1'000'000'000ULL;

const char *const kCtrlB = "\x02";
const char *const kCtrlD = "\x04";
const char *const kCtrlF = "\x06";
const char *const kCtrlU = "\x15";

// A printable keystroke is anything that is not a C0 control byte. Note the
// cast: `char` is signed, so a UTF-8 lead byte such as 0xC3 ('é') is negative
// and must never be compared or fed to <cctype> as a plain char.
bool IsPrintableInput(const std::string &input) {
  if (input.empty())
    return false;
  const unsigned char first = static_cast<unsigned char>(input[0]);
  return first >= 0x20 && first != 0x7F;
}

bool IsAsciiDigit(char c) {
  const unsigned char b = static_cast<unsigned char>(c);
  return b >= '0' && b <= '9';
}

std::string Base64(const std::string &input) {
  static const char *table =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((input.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < input.size()) {
    const unsigned value = (static_cast<unsigned char>(input[i]) << 16) |
                           (static_cast<unsigned char>(input[i + 1]) << 8) |
                           static_cast<unsigned char>(input[i + 2]);
    out.push_back(table[(value >> 18) & 0x3F]);
    out.push_back(table[(value >> 12) & 0x3F]);
    out.push_back(table[(value >> 6) & 0x3F]);
    out.push_back(table[value & 0x3F]);
    i += 3;
  }
  if (i < input.size()) {
    unsigned value = static_cast<unsigned char>(input[i]) << 16;
    const bool has_second = i + 1 < input.size();
    if (has_second)
      value |= static_cast<unsigned char>(input[i + 1]) << 8;
    out.push_back(table[(value >> 18) & 0x3F]);
    out.push_back(table[(value >> 12) & 0x3F]);
    out.push_back(has_second ? table[(value >> 6) & 0x3F] : '=');
    out.push_back('=');
  }
  return out;
}

std::string FormatDouble(double value) {
  std::ostringstream out;
  out << std::setprecision(6) << std::noshowpoint << value;
  return out.str();
}

} // namespace

CSVController::CSVController(CSVModel &model, CSVView &view,
                             ScreenInteractive &screen)
    : model_(model), view_(view), screen_(screen) {
  SyncView();

  component_ = CatchEvent(Renderer([this] {
                            // Recomputing here is what keeps the view correct
                            // when the terminal is resized: FTXUI re-renders on
                            // SIGWINCH without delivering a key event.
                            ClampToView();
                            SyncView();
                            const auto size = Terminal::Size();
                            return view_.Render(size.dimx, size.dimy);
                          }),
                          [this](Event event) { return OnEvent(event); });
}

Component CSVController::GetComponent() { return component_; }

int CSVController::VisibleRows() const {
  return std::max(1, view_.RowsThatFit(Terminal::Size().dimy));
}

int CSVController::TerminalWidth() const {
  return std::max(8, Terminal::Size().dimx);
}

// --- cursor bookkeeping -----------------------------------------------------

bool CSVController::RowExists(size_t row) {
  std::vector<std::string> fields;
  return model_.GetRow(row, fields);
}

size_t CSVController::KnownLastRow() {
  const size_t count = model_.RowCount();
  return count == 0 ? 0 : count - 1;
}

void CSVController::MoveCursorRows(long long delta) {
  if (delta == 0)
    return;

  if (delta < 0) {
    const size_t magnitude = static_cast<size_t>(-delta);
    cursor_row_ = magnitude > cursor_row_ ? 0 : cursor_row_ - magnitude;
  } else {
    const size_t target = cursor_row_ + static_cast<size_t>(delta);
    // Probing avoids scanning the whole file just to move one row down.
    if (RowExists(target))
      cursor_row_ = target;
    else
      cursor_row_ = KnownLastRow();
  }
  ClampToView();
}

void CSVController::MoveCursorColumns(long long delta) {
  const size_t columns = std::max<size_t>(model_.ColumnCount(), 1);
  long long target = static_cast<long long>(cursor_col_) + delta;
  target = std::clamp<long long>(target, 0, static_cast<long long>(columns) - 1);
  cursor_col_ = static_cast<size_t>(target);
  view_.EnsureColumnVisible(cursor_col_, TerminalWidth());
}

void CSVController::GoToRow(size_t row) {
  const size_t last = KnownLastRow();
  cursor_row_ = std::min(row, last);
  ClampToView();
}

void CSVController::ClampToView() {
  const int visible = VisibleRows();
  if (cursor_row_ < start_row_)
    start_row_ = cursor_row_;
  else if (cursor_row_ >= start_row_ + static_cast<size_t>(visible))
    start_row_ = cursor_row_ - static_cast<size_t>(visible) + 1;
}

void CSVController::SyncView() {
  view_.SetCursor(cursor_row_, cursor_col_);
  view_.SetViewportStart(start_row_);
}

void CSVController::SetMessage(const std::string &message, bool is_error) {
  view_.SetStatusMessage(message, is_error);
}

void CSVController::UpdateCommandLine() {
  switch (input_mode_) {
  case InputMode::Search:
    view_.SetCommandLine("/" + input_buffer_);
    break;
  case InputMode::Filter:
    view_.SetCommandLine("filter: " + input_buffer_);
    break;
  case InputMode::Normal:
    view_.SetCommandLine(std::string());
    break;
  }
}

size_t CSVController::ConsumeCount() {
  const size_t count = pending_count_ == 0 ? 1 : pending_count_;
  pending_count_ = 0;
  awaiting_second_g_ = false;
  return count;
}

std::string CSVController::CurrentCellValue() {
  std::vector<std::string> fields;
  if (!model_.GetRow(cursor_row_, fields))
    return std::string();
  return cursor_col_ < fields.size() ? fields[cursor_col_] : std::string();
}

// --- actions ----------------------------------------------------------------

void CSVController::RunSearch(const std::string &pattern, bool forward,
                              bool from_cursor) {
  if (pattern.empty())
    return;

  const size_t row = from_cursor ? cursor_row_ : 0;
  const size_t col = from_cursor ? cursor_col_ : 0;
  auto hit = forward ? model_.FindNext(pattern, row, col, true)
                     : model_.FindPrev(pattern, row, col, true);

  view_.SetSearch(pattern);
  if (!hit) {
    view_.SetCurrentMatch(std::nullopt, std::nullopt);
    SetMessage("not found: " + pattern, true);
    return;
  }

  const bool wrapped = forward ? (hit->row < row || (hit->row == row && hit->col <= col))
                               : (hit->row > row || (hit->row == row && hit->col >= col));
  cursor_row_ = hit->row;
  cursor_col_ = hit->col;
  view_.EnsureColumnVisible(cursor_col_, TerminalWidth());
  view_.SetCurrentMatch(hit->row, hit->col);
  ClampToView();
  SetMessage(wrapped ? (forward ? "search wrapped to top" : "search wrapped to bottom")
                     : std::string());
  last_search_ = pattern;
}

void CSVController::RepeatSearch(bool forward) {
  if (!last_search_) {
    SetMessage("no previous search", true);
    return;
  }
  RunSearch(*last_search_, forward, true);
}

void CSVController::ApplyFilter(const std::string &pattern) {
  if (pattern.empty()) {
    model_.ClearFilter();
    SetMessage("filter cleared");
  } else {
    const size_t matches = model_.ApplyFilter(pattern);
    SetMessage(std::to_string(matches) + " row(s) match");
  }
  cursor_row_ = 0;
  start_row_ = 0;
  ClampToView();
}

void CSVController::SortByCursorColumn(bool descending) {
  model_.SortByColumn(cursor_col_, descending);
  cursor_row_ = 0;
  start_row_ = 0;
  SetMessage("sorted by " + model_.ColumnName(cursor_col_) +
             (descending ? " (desc)" : " (asc)"));
}

void CSVController::ClearOrdering() {
  model_.ClearSort();
  model_.ClearFilter();
  view_.ShowAllColumns();
  cursor_row_ = std::min(cursor_row_, KnownLastRow());
  ClampToView();
  SetMessage("sort and filter cleared");
}

void CSVController::YankCurrentCell() {
  const std::string value = CurrentCellValue();
  // OSC 52 puts it on the system clipboard, and works over SSH.
  std::cout << "\033]52;c;" << Base64(value) << "\a" << std::flush;
  SetMessage("copied " + std::to_string(value.size()) + " byte(s)");
}

void CSVController::ShowColumnStats() {
  const auto stats = model_.ComputeColumnStats(cursor_col_);
  std::ostringstream out;
  out << model_.ColumnName(cursor_col_) << ": " << stats.total << " rows, "
      << stats.empty << " empty";
  if (stats.numeric > 0) {
    out << ", min " << FormatDouble(stats.min) << ", max "
        << FormatDouble(stats.max) << ", mean " << FormatDouble(stats.mean);
  } else {
    out << ", non-numeric";
  }
  SetMessage(out.str());
}

// --- event handling ---------------------------------------------------------

bool CSVController::OnEvent(Event event) {
  if (event.is_mouse())
    return OnMouseEvent(event);

  if (view_.AnyOverlayVisible())
    return OnOverlayEvent(event);

  if (input_mode_ != InputMode::Normal)
    return OnTextInputEvent(event);

  return OnNormalEvent(event);
}

bool CSVController::OnOverlayEvent(Event event) {
  if (event == Event::Character('q')) {
    screen_.Exit();
    return true;
  }
  if (event == Event::Escape || event == Event::Return ||
      event == Event::Character('?')) {
    view_.CloseOverlays();
    return true;
  }
  return true; // swallow everything else while an overlay is up
}

bool CSVController::OnTextInputEvent(Event event) {
  if (event == Event::Return) {
    const std::string pattern = input_buffer_;
    const InputMode mode = input_mode_;
    input_mode_ = InputMode::Normal;
    input_buffer_.clear();
    UpdateCommandLine();

    if (mode == InputMode::Search)
      RunSearch(pattern, true, true);
    else
      ApplyFilter(pattern);
    return true;
  }

  if (event == Event::Escape) {
    input_mode_ = InputMode::Normal;
    input_buffer_.clear();
    UpdateCommandLine();
    return true;
  }

  if (event == Event::Backspace) {
    if (!input_buffer_.empty()) {
      // Erase a whole UTF-8 character, not a single byte.
      input_buffer_.resize(csv::PrevCharBoundary(input_buffer_, input_buffer_.size()));
      UpdateCommandLine();
    } else {
      input_mode_ = InputMode::Normal;
      UpdateCommandLine();
    }
    return true;
  }

  if (event.is_character() && IsPrintableInput(event.input())) {
    // Append the whole character. Taking only input()[0] would split a
    // multi-byte character and corrupt the pattern.
    input_buffer_ += event.input();
    UpdateCommandLine();
    return true;
  }

  return true;
}

bool CSVController::OnNormalEvent(Event event) {
  const int page = VisibleRows();

  // Numeric prefix for vim-style motions.
  if (event.is_character() && event.input().size() == 1 &&
      IsAsciiDigit(event.input()[0])) {
    const char digit = event.input()[0];
    if (digit == '0' && pending_count_ == 0) {
      // Bare 0 means "first column".
      cursor_col_ = 0;
      view_.EnsureColumnVisible(0, TerminalWidth());
      return true;
    }
    if (pending_count_ < kMaxCount)
      pending_count_ = pending_count_ * 10 + static_cast<size_t>(digit - '0');
    awaiting_second_g_ = false;
    return true;
  }

  if (event == Event::Character('/')) {
    input_mode_ = InputMode::Search;
    input_buffer_.clear();
    UpdateCommandLine();
    return true;
  }
  if (event == Event::Character('f')) {
    input_mode_ = InputMode::Filter;
    input_buffer_ = model_.filter_pattern();
    UpdateCommandLine();
    return true;
  }

  if (event == Event::Character('q')) {
    screen_.Exit();
    return true;
  }
  if (event == Event::Escape) {
    view_.SetSearch(std::string());
    view_.SetCurrentMatch(std::nullopt, std::nullopt);
    SetMessage(std::string());
    pending_count_ = 0;
    awaiting_second_g_ = false;
    return true;
  }

  if (event == Event::Character('g')) {
    if (awaiting_second_g_) {
      const size_t target = pending_count_ == 0 ? 0 : pending_count_ - 1;
      pending_count_ = 0;
      awaiting_second_g_ = false;
      GoToRow(target);
    } else {
      awaiting_second_g_ = true;
    }
    return true;
  }

  if (event == Event::Character('G')) {
    const size_t target =
        pending_count_ == 0 ? KnownLastRow() : pending_count_ - 1;
    pending_count_ = 0;
    awaiting_second_g_ = false;
    GoToRow(target);
    return true;
  }

  if (event == Event::Character('j') || event == Event::ArrowDown) {
    MoveCursorRows(static_cast<long long>(ConsumeCount()));
    return true;
  }
  if (event == Event::Character('k') || event == Event::ArrowUp) {
    MoveCursorRows(-static_cast<long long>(ConsumeCount()));
    return true;
  }
  if (event == Event::Character('l') || event == Event::ArrowRight) {
    MoveCursorColumns(static_cast<long long>(ConsumeCount()));
    return true;
  }
  if (event == Event::Character('h') || event == Event::ArrowLeft) {
    MoveCursorColumns(-static_cast<long long>(ConsumeCount()));
    return true;
  }

  if (event == Event::Character('$') || event == Event::End) {
    const size_t columns = std::max<size_t>(model_.ColumnCount(), 1);
    cursor_col_ = columns - 1;
    view_.EnsureColumnVisible(cursor_col_, TerminalWidth());
    return true;
  }
  if (event == Event::Home) {
    cursor_col_ = 0;
    view_.EnsureColumnVisible(0, TerminalWidth());
    return true;
  }

  if (event.input() == kCtrlD) {
    MoveCursorRows(std::max(1, page / 2));
    return true;
  }
  if (event.input() == kCtrlU) {
    MoveCursorRows(-std::max(1, page / 2));
    return true;
  }
  if (event == Event::PageDown || event.input() == kCtrlF) {
    MoveCursorRows(page);
    return true;
  }
  if (event == Event::PageUp || event.input() == kCtrlB) {
    MoveCursorRows(-page);
    return true;
  }

  if (event == Event::Character('n')) {
    RepeatSearch(true);
    return true;
  }
  if (event == Event::Character('N')) {
    RepeatSearch(false);
    return true;
  }

  if (event == Event::Character('s')) {
    SortByCursorColumn(false);
    return true;
  }
  if (event == Event::Character('S')) {
    SortByCursorColumn(true);
    return true;
  }
  if (event == Event::Character('u')) {
    ClearOrdering();
    return true;
  }

  if (event == Event::Character('x')) {
    view_.ToggleColumnHidden(cursor_col_);
    SetMessage("hid " + model_.ColumnName(cursor_col_) + " (X shows all)");
    return true;
  }
  if (event == Event::Character('X')) {
    view_.ShowAllColumns();
    SetMessage("all columns shown");
    return true;
  }
  if (event == Event::Character('z')) {
    const size_t frozen = view_.FrozenColumns() == cursor_col_ + 1 ? 0 : cursor_col_ + 1;
    view_.SetFrozenColumns(frozen);
    SetMessage(frozen == 0 ? "columns unfrozen"
                           : "froze " + std::to_string(frozen) + " column(s)");
    return true;
  }

  if (event == Event::Character('H')) {
    view_.ToggleHeaderPinned();
    ClampToView();
    return true;
  }
  if (event == Event::Character('t')) {
    view_.ToggleTabularMode();
    return true;
  }
  if (event == Event::Character('?')) {
    view_.ToggleHelp();
    return true;
  }
  if (event == Event::Return) {
    view_.ToggleCellDetail();
    return true;
  }
  if (event == Event::Character('y')) {
    YankCurrentCell();
    return true;
  }
  if (event == Event::Character('c')) {
    ShowColumnStats();
    return true;
  }

  pending_count_ = 0;
  awaiting_second_g_ = false;
  return false;
}

bool CSVController::OnMouseEvent(Event event) {
  const auto &mouse = event.mouse();

  if (mouse.button == Mouse::WheelUp) {
    MoveCursorRows(-3);
    return true;
  }
  if (mouse.button == Mouse::WheelDown) {
    MoveCursorRows(3);
    return true;
  }

  if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed) {
    if (view_.AnyOverlayVisible()) {
      view_.CloseOverlays();
      return true;
    }
    const int header_rows =
        (view_.HeaderPinned() && !model_.Header().empty()) ? 2 : 0;
    const int clicked = mouse.y - header_rows;
    if (clicked >= 0) {
      const size_t target = start_row_ + static_cast<size_t>(clicked);
      if (RowExists(target))
        cursor_row_ = target;
    }
    return true;
  }
  return false;
}
