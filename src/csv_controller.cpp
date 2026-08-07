#include "csv_controller.h"

#include "csv_model.h"
#include "csv_parser.h"
#include "csv_system.h"
#include "csv_view.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <cmath>
#include <memory>
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

// Braille dots, which are a single cell wide and turn smoothly. The point of
// them is motion rather than information: the percentage says how far along a
// pass is, and this says that it is still going.
const char *const kSpinner[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
constexpr size_t kSpinnerFrames = sizeof(kSpinner) / sizeof(kSpinner[0]);

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
                            // While a search runs the model belongs to the
                            // worker, so this must not read it: draw the frame
                            // from before the search instead.
                            PollBlocking();
                            if (blocking_ != Blocking::None)
                              return RenderWhileBlocked();

                            // Recomputing here is what keeps the view correct
                            // when the terminal is resized: FTXUI re-renders on
                            // SIGWINCH without delivering a key event.
                            PollScanner();
                            ClampToView();
                            SyncView();
                            const auto size = Terminal::Size();
                            return view_.Render(size.dimx, size.dimy);
                          }),
                          [this](Event event) { return OnEvent(event); });
}

CSVController::~CSVController() {
  // The worker holds a reference to the model and posts to the screen; both
  // outlive it only if it is stopped first.
  blocking_cancel_.store(true, std::memory_order_release);
  JoinBlocking();
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

size_t CSVController::LastRowBetween(size_t low, size_t high) {
  // Binary search on existence. Every probe lands within a few chunks of one
  // we have already resolved, so this never degrades into a full scan the way
  // asking for the exact row count would.
  if (!RowExists(low))
    return low;
  while (low < high) {
    const size_t mid = low + (high - low + 1) / 2;
    if (RowExists(mid))
      low = mid;
    else
      high = mid - 1;
  }
  return low;
}

void CSVController::MoveCursorRows(long long delta) {
  if (delta == 0)
    return;

  if (delta < 0) {
    const size_t magnitude = static_cast<size_t>(-delta);
    cursor_row_ = magnitude > cursor_row_ ? 0 : cursor_row_ - magnitude;
  } else {
    const size_t target = cursor_row_ + static_cast<size_t>(delta);
    if (RowExists(target)) {
      cursor_row_ = target;
    } else if (model_.RowCountKnown()) {
      cursor_row_ = KnownLastRow();
    } else {
      // The end of the file is somewhere in (cursor, target]; find it with
      // probes rather than counting every row in the file.
      cursor_row_ = LastRowBetween(cursor_row_, target);
    }
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
  if (RowExists(row)) {
    cursor_row_ = row;
  } else if (model_.RowCountKnown()) {
    cursor_row_ = KnownLastRow();
  } else {
    RequestExactCount(Task::Row, row);
    return;
  }
  ClampToView();
}

// --- background passes ------------------------------------------------------
//
// Counting, sorting, filtering and column statistics all need to read every
// row, so they all run here: on a worker thread, reporting progress, and
// abandonable with Esc. Nothing below blocks the UI.

void CSVController::StartScan(Task task, const csvscan::Request &request,
                              const std::string &label) {
  // A second request supersedes the first rather than queueing behind it.
  if (scanner_.running()) {
    scanner_.Cancel();
    scanner_.Join();
  }

  task_ = task;
  task_label_ = label;
  scanner_.Start(request, [this] {
    // Called from the worker thread; waking the loop is the only cross-thread
    // interaction, and PostEvent is the one FTXUI call that allows it.
    screen_.PostEvent(ftxui::Event::Custom);
  });
  SetMessage(label + "… Esc to cancel");
}

void CSVController::RequestExactCount(Task task, size_t row) {
  task_row_ = row;

  if (model_.RowCountKnown()) {
    // Nothing to read; apply straight away.
    task_ = task;
    csvscan::Result empty;
    FinishScan(empty);
    return;
  }

  csvscan::Request request;
  model_.DescribeScan(request);
  StartScan(task, request, "counting rows");
}

bool CSVController::CancelScan() {
  if (!scanner_.running())
    return false;
  // The worker only notices between rows, so it keeps running for a moment
  // yet. Say so now: pressing Esc and watching the percentage climb on is
  // indistinguishable from Esc not working.
  scanner_.Cancel();
  task_ = Task::None;
  cancel_requested_ = true;
  SetMessage("stopping " + task_label_ + "…");
  return true;
}

void CSVController::PollScanner() {
  if (scanner_.running()) {
    // Leave the "stopping" message alone rather than overwriting it with a
    // progress reading the user has just asked to be rid of.
    if (cancel_requested_)
      return;

    const int percent = static_cast<int>(scanner_.progress() * 100.0);
    const bool merging = scanner_.phase() == csvscan::Phase::Merging;
    const bool filtering = scanner_.request().filter &&
                           !scanner_.request().filter_pattern.empty();
    // While filtering, the useful number is how many rows survived, not how
    // many were read; while merging it is how many are in their final place.
    const std::string count =
        merging ? csv::HumanCount(scanner_.rows_kept()) + " placed"
        : filtering ? csv::HumanCount(scanner_.rows_kept()) + " matched"
                    : csv::HumanCount(scanner_.rows_seen()) + " rows";

    // The spinner turns on every frame, not on every percent. A file whose
    // rows are cheap to read can sit on one number for a while, and a static
    // number is indistinguishable from a program that has stopped.
    spinner_frame_ = (spinner_frame_ + 1) % kSpinnerFrames;
    const std::string spinner(kSpinner[spinner_frame_]);

    SetMessage(spinner + " " + task_label_ +
               (merging ? " — merging " : "… ") + std::to_string(percent) +
               "%  (" + count + ", Esc to cancel)");
    return;
  }

  if (scanner_.state() == CSVScanner::State::Failed) {
    scanner_.Join();
    // A sort that ran out of temporary disk space has something specific to
    // say; anything else was a problem reading the file itself.
    const std::string &reason = scanner_.error();
    SetMessage(reason.empty() ? "could not read " + model_.path() : reason, true);
    task_ = Task::None;
    cancel_requested_ = false;
    return;
  }

  if (cancel_requested_ && scanner_.state() == CSVScanner::State::Cancelled) {
    scanner_.Join();
    cancel_requested_ = false;
    SetMessage(task_label_ + " cancelled", true);
    return;
  }

  csvscan::Result result;
  if (!scanner_.Take(result))
    return; // idle, or cancelled before finishing
  if (task_ == Task::None) {
    // A pass whose result nobody is waiting for — but it still counted the
    // file on its way through, so keep that much.
    model_.AdoptIndex(std::move(result.offsets), result.total_rows);
    model_.SaveIndex();
    cancel_requested_ = false;
    return;
  }

  // Every full pass yields the offset table and the exact row count, whatever
  // it was actually asked for. Keeping it means the next session starts with
  // the file already counted.
  model_.AdoptIndex(std::move(result.offsets), result.total_rows);
  model_.SaveIndex();
  FinishScan(result);
}

void CSVController::FinishScan(csvscan::Result &result) {
  const Task task = task_;
  task_ = Task::None;

  switch (task) {
  case Task::End:
    cursor_row_ = KnownLastRow();
    SetMessage(csv::HumanCount(model_.RowCount()) + " rows");
    break;
  case Task::Row:
    cursor_row_ = std::min(task_row_, KnownLastRow());
    SetMessage(std::string());
    break;
  case Task::Sort:
    model_.AdoptView(task_view_, std::move(result.order), result.has_order);
    cursor_row_ = 0;
    start_row_ = 0;
    SetMessage("sorted by " + model_.ColumnName(task_view_.sort_column) +
               (task_view_.sort_descending ? " (desc)" : " (asc)"));
    break;
  case Task::Filter:
    model_.AdoptView(task_view_, std::move(result.order), result.has_order);
    cursor_row_ = 0;
    start_row_ = 0;
    SetMessage(task_view_.filter_active
                   ? csv::HumanCount(model_.RowCount()) + " row(s) match"
                   : std::string("filter cleared"));
    break;
  case Task::Stats:
    SetMessage(DescribeStats(task_column_, result.stats));
    break;
  case Task::MatchCount:
    match_total_ = result.matches;
    match_total_known_ = true;
    // Said rather than shouted: the jump already happened, and this is the
    // number that could not be known at the time.
    SetMessage(csv::HumanCount(match_total_) + " match(es) for '" +
               match_pattern_ + "'");
    break;
  case Task::None:
    break;
  }
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
  case InputMode::Export:
    view_.SetCommandLine("write to: " + input_buffer_);
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

void CSVController::StartBlocking(Blocking kind, const std::string &label,
                                  std::function<void()> body) {
  if (blocking_ != Blocking::None)
    return;
  JoinBlocking(); // reap a finished one

  blocking_ = kind;
  blocking_label_ = label;
  blocking_message_ = label + "…";
  blocking_cancel_.store(false, std::memory_order_release);
  blocking_finished_.store(false, std::memory_order_release);
  blocking_progress_.store(0, std::memory_order_relaxed);

  // Whatever was on screen when the work began is what stays on screen: the
  // model belongs to the worker now, and rendering the grid would read it.
  const auto size = Terminal::Size();
  frozen_frame_ = view_.Render(size.dimx, size.dimy);

  blocking_thread_ = std::thread([this, body = std::move(body)] {
    body();
    blocking_finished_.store(true, std::memory_order_release);
    screen_.PostEvent(ftxui::Event::Custom);
  });
}

void CSVController::JoinBlocking() {
  if (blocking_thread_.joinable())
    blocking_thread_.join();
}

bool CSVController::BlockingCancelled() const {
  return blocking_cancel_.load(std::memory_order_acquire);
}

std::function<void(size_t)> CSVController::BlockingReporter() {
  auto last_wake = std::make_shared<std::chrono::steady_clock::time_point>(
      std::chrono::steady_clock::now());
  return [this, last_wake](size_t done) {
    blocking_progress_.store(done, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    if (now - *last_wake < std::chrono::milliseconds(100))
      return;
    *last_wake = now;
    screen_.PostEvent(ftxui::Event::Custom);
  };
}

bool CSVController::CancelBlocking() {
  if (blocking_ == Blocking::None)
    return false;
  blocking_cancel_.store(true, std::memory_order_release);
  blocking_message_ = "stopping " + blocking_label_ + "…";
  return true;
}

void CSVController::StartSearch(const std::string &pattern, bool forward,
                                bool from_cursor) {
  if (pattern.empty() || blocking_ != Blocking::None)
    return;

  const size_t row = from_cursor ? cursor_row_ : 0;
  const size_t col = from_cursor ? cursor_col_ : 0;

  search_pattern_ = pattern;
  search_forward_ = forward;
  search_origin_row_ = row;
  search_origin_col_ = col;
  search_hit_.reset();

  CSVModel::SearchWatch watch;
  watch.cancelled = [this] { return BlockingCancelled(); };
  watch.report = BlockingReporter();

  StartBlocking(Blocking::Search, "searching for '" + pattern + "'",
                [this, pattern, forward, row, col, watch] {
                  search_hit_ =
                      forward ? model_.FindNext(pattern, row, col, true, watch)
                              : model_.FindPrev(pattern, row, col, true, watch);
                });
}

void CSVController::StartExport(const std::string &path) {
  if (path.empty() || blocking_ != Blocking::None)
    return;

  // Refusing an existing file rather than prompting again: this is the one
  // place csvtui writes anything, and quietly replacing a file the user
  // already has would be the worst possible first impression of that.
  struct stat info {};
  if (::stat(path.c_str(), &info) == 0) {
    SetMessage(path + " already exists — pick another name", true);
    return;
  }

  export_path_ = path;
  export_error_.clear();

  // The columns on screen, in order. Hiding a column is part of the view, so
  // the file written out matches what was being looked at.
  std::vector<size_t> columns;
  for (size_t col = 0; col < model_.ColumnCount(); ++col) {
    if (!view_.ColumnHidden(col))
      columns.push_back(col);
  }
  if (columns.empty()) {
    SetMessage("every column is hidden — nothing to write", true);
    return;
  }

  auto report = BlockingReporter();
  StartBlocking(Blocking::Export, "writing " + path,
                [this, path, columns, report] {
                  export_error_ = WriteViewTo(path, columns, report);
                });
}

std::string CSVController::WriteViewTo(const std::string &path,
                                       const std::vector<size_t> &columns,
                                       const std::function<void(size_t)> &report) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open())
    return "cannot write " + path + ": " + std::strerror(errno);

  const char delimiter = model_.delimiter();
  std::string line;

  if (model_.has_header()) {
    const std::vector<std::string> &header = model_.Header();
    for (size_t i = 0; i < columns.size(); ++i) {
      if (i != 0)
        line.push_back(delimiter);
      const size_t col = columns[i];
      csv::AppendQuoted(line, col < header.size() ? header[col] : std::string(),
                        delimiter);
    }
    line.push_back('\n');
    out << line;
  }

  std::vector<std::string> fields;
  size_t written = 0;

  // Walk the view until it runs out rather than asking how long it is: with a
  // sort or filter in effect the ordering already knows where it ends, and
  // without one this avoids counting the file just to write it.
  for (size_t row = 0; model_.GetRow(row, fields); ++row) {
    if (BlockingCancelled()) {
      out.close();
      ::unlink(path.c_str());
      return std::string();
    }

    line.clear();
    for (size_t i = 0; i < columns.size(); ++i) {
      if (i != 0)
        line.push_back(delimiter);
      const size_t col = columns[i];
      csv::AppendQuoted(line, col < fields.size() ? fields[col] : std::string(),
                        delimiter);
    }
    line.push_back('\n');
    out << line;
    if (!out) {
      out.close();
      ::unlink(path.c_str());
      return "writing " + path + " failed, probably out of disk space";
    }

    ++written;
    if (report)
      report(written);
  }

  out.flush();
  if (!out) {
    out.close();
    ::unlink(path.c_str());
    return "writing " + path + " failed, probably out of disk space";
  }
  if (report)
    report(written);
  return std::string();
}

void CSVController::PollBlocking() {
  if (blocking_ == Blocking::None)
    return;

  if (!blocking_finished_.load(std::memory_order_acquire)) {
    spinner_frame_ = (spinner_frame_ + 1) % kSpinnerFrames;
    const size_t done = blocking_progress_.load(std::memory_order_relaxed);
    const char *unit = blocking_ == Blocking::Export ? " rows written" : " rows";
    blocking_message_ = std::string(kSpinner[spinner_frame_]) + " " +
                        blocking_label_ + "  (" + csv::HumanCount(done) + unit +
                        (BlockingCancelled() ? ", stopping…" : ", Esc to cancel") +
                        ")";
    return;
  }

  JoinBlocking();
  const Blocking kind = blocking_;
  const bool cancelled = BlockingCancelled();
  blocking_ = Blocking::None;
  frozen_frame_ = ftxui::Element();

  switch (kind) {
  case Blocking::Search: {
    auto hit = search_hit_;
    search_hit_.reset();
    if (cancelled) {
      SetMessage("search cancelled", true);
      break;
    }
    ApplySearchHit(search_pattern_, search_forward_, search_origin_row_,
                   search_origin_col_, hit);
    break;
  }
  case Blocking::Export: {
    const size_t written = blocking_progress_.load(std::memory_order_relaxed);
    if (cancelled) {
      SetMessage("writing cancelled — " + export_path_ + " removed", true);
    } else if (!export_error_.empty()) {
      SetMessage(export_error_, true);
    } else {
      SetMessage("wrote " + csv::HumanCount(written) + " row(s) to " +
                 export_path_);
    }
    break;
  }
  case Blocking::None:
    break;
  }
}

ftxui::Element CSVController::RenderWhileBlocked() {
  // The frame from before the work began, dimmed, with a live progress line
  // laid over its last row. Dimming is the honest signal: the grid is real,
  // and it is not going to answer you until this finishes.
  Element grid = frozen_frame_ ? (frozen_frame_ | dim) : filler();
  Element line = vbox({filler(), text(blocking_message_) | color(Color::Yellow)});
  return dbox({std::move(grid), std::move(line)});
}

void CSVController::RunSearch(const std::string &pattern, bool forward,
                              bool from_cursor) {
  if (pattern.empty())
    return;

  const size_t row = from_cursor ? cursor_row_ : 0;
  const size_t col = from_cursor ? cursor_col_ : 0;
  auto hit = forward ? model_.FindNext(pattern, row, col, true)
                     : model_.FindPrev(pattern, row, col, true);
  ApplySearchHit(pattern, forward, row, col, hit);
}

void CSVController::ApplySearchHit(const std::string &pattern, bool forward,
                                   size_t row, size_t col,
                                   const std::optional<CSVModel::SearchHit> &hit) {
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
  StartMatchCount(pattern);
}

void CSVController::RepeatSearch(bool forward) {
  if (!last_search_) {
    SetMessage("no previous search", true);
    return;
  }
  StartSearch(*last_search_, forward, true);
}

void CSVController::ApplyFilter(const std::string &pattern) {
  CSVModel::ViewState target = model_.CurrentViewState();
  target.filter_active = !pattern.empty();
  target.filter_pattern = pattern;

  // Dropping the filter from an otherwise unordered view needs no work at all:
  // the view becomes the file again.
  if (!target.filter_active && !target.sort_active) {
    model_.AdoptView(target, {}, false);
    cursor_row_ = 0;
    start_row_ = 0;
    ClampToView();
    SetMessage("filter cleared");
    return;
  }

  csvscan::Request request;
  model_.DescribeScan(request);
  request.filter = target.filter_active;
  request.filter_pattern = target.filter_pattern;
  request.sort = target.sort_active;
  request.sort_column = target.sort_column;
  request.sort_descending = target.sort_descending;
  request.want_order = true;

  task_view_ = target;
  StartScan(Task::Filter, request,
            target.filter_active ? "filtering" : "restoring order");
}

void CSVController::SortByCursorColumn(bool descending) {
  CSVModel::ViewState target = model_.CurrentViewState();
  target.sort_active = true;
  target.sort_column = cursor_col_;
  target.sort_descending = descending;

  csvscan::Request request;
  model_.DescribeScan(request);
  request.filter = target.filter_active;
  request.filter_pattern = target.filter_pattern;
  request.sort = true;
  request.sort_column = target.sort_column;
  request.sort_descending = descending;
  request.want_order = true;

  task_view_ = target;
  StartScan(Task::Sort, request, "sorting by " + model_.ColumnName(cursor_col_));
}

void CSVController::ClearOrdering() {
  if (scanner_.running()) {
    scanner_.Cancel();
    scanner_.Join();
    task_ = Task::None;
  }
  model_.AdoptView(CSVModel::ViewState{}, {}, false);
  view_.ShowAllColumns();
  cursor_row_ = 0;
  start_row_ = 0;
  ClampToView();
  SetMessage("sort and filter cleared");
}

void CSVController::YankCurrentCell() {
  const std::string value = CurrentCellValue();
  // OSC 52 puts it on the system clipboard, and works over SSH.
  std::cout << "\033]52;c;" << Base64(value) << "\a" << std::flush;
  SetMessage("copied " + std::to_string(value.size()) + " byte(s)");
}

void CSVController::StartMatchCount(const std::string &pattern) {
  // Only worth doing once per pattern, and never at the cost of interrupting
  // something the user actually asked for.
  if (pattern.empty() || scanner_.running())
    return;
  if (match_total_known_ && match_pattern_ == pattern)
    return;

  match_pattern_ = pattern;
  match_total_known_ = false;

  csvscan::Request request;
  model_.DescribeScan(request);
  const CSVModel::ViewState view = model_.CurrentViewState();
  request.filter = view.filter_active;
  request.filter_pattern = view.filter_pattern;
  request.count_pattern = pattern;

  StartScan(Task::MatchCount, request, "counting matches for '" + pattern + "'");
}

void CSVController::ShowColumnStats() {
  const CSVModel::ViewState view = model_.CurrentViewState();

  csvscan::Request request;
  model_.DescribeScan(request);
  // Statistics describe the rows on screen, so an active filter applies. A
  // sort does not: it reorders the same set.
  request.filter = view.filter_active;
  request.filter_pattern = view.filter_pattern;
  request.want_stats = true;
  request.stats_column = cursor_col_;

  task_column_ = cursor_col_;
  StartScan(Task::Stats, request, "scanning " + model_.ColumnName(cursor_col_));
}

std::string CSVController::DescribeStats(size_t col,
                                         const csvscan::Stats &stats) const {
  std::ostringstream out;
  out << model_.ColumnName(col) << ": " << csv::HumanCount(stats.total)
      << " rows, " << csv::HumanCount(stats.empty) << " empty";
  if (stats.numeric > 0) {
    out << ", min " << FormatDouble(stats.min) << ", max "
        << FormatDouble(stats.max) << ", mean " << FormatDouble(stats.mean);
  } else {
    out << ", non-numeric";
  }
  return out.str();
}

// --- event handling ---------------------------------------------------------

bool CSVController::OnEvent(Event event) {
  // A worker owns the model while it runs, so nothing here may touch it. Esc
  // stops it; everything else waits, which is also the only sensible answer to
  // "move the cursor" while something is looking for where to put the cursor.
  if (blocking_ != Blocking::None) {
    if (event == Event::Escape)
      CancelBlocking();
    return true;
  }

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

    if (mode == InputMode::Export) {
      StartExport(pattern);
    } else if (mode == InputMode::Search) {
      StartSearch(pattern, true, true);
    } else if (pattern.empty()) {
      ApplyFilter(pattern); // clearing never costs anything
    } else {
      const std::string blocked = model_.CheckFilterFeasible();
      if (!blocked.empty())
        SetMessage(blocked, true);
      else
        ApplyFilter(pattern);
    }
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
  if (event == Event::Character('<') || event == Event::Character('>')) {
    const int step = event == Event::Character('>') ? 4 : -4;
    view_.WidenColumn(cursor_col_, step * static_cast<int>(ConsumeCount()));
    view_.EnsureColumnVisible(cursor_col_, TerminalWidth());
    SetMessage(model_.ColumnName(cursor_col_) + " width " +
               std::to_string(view_.ColumnWidth(cursor_col_)));
    return true;
  }
  if (event == Event::Character('=')) {
    view_.FitColumnToScreen(cursor_col_, Terminal::Size().dimy);
    view_.EnsureColumnVisible(cursor_col_, TerminalWidth());
    SetMessage(model_.ColumnName(cursor_col_) + " fitted to " +
               std::to_string(view_.ColumnWidth(cursor_col_)));
    return true;
  }

  if (event == Event::Character('w')) {
    input_mode_ = InputMode::Export;
    input_buffer_.clear();
    UpdateCommandLine();
    return true;
  }

  if (event == Event::Character('q')) {
    screen_.Exit();
    return true;
  }
  if (event == Event::Escape) {
    if (CancelScan())
      return true;
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
    const size_t count = pending_count_;
    pending_count_ = 0;
    awaiting_second_g_ = false;
    if (count != 0)
      GoToRow(count - 1);
    else if (model_.RowCountKnown())
      GoToRow(KnownLastRow());
    else
      // Jumping to the end is the one motion that genuinely needs the row
      // count, so hand it to the background scan instead of freezing.
      RequestExactCount(Task::End);
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

  if (event == Event::Character('s') || event == Event::Character('S')) {
    const bool descending = event == Event::Character('S');
    // Sorting holds an index for every row, so on a very large file it can ask
    // for more memory than the machine has. Refuse with numbers rather than
    // letting the allocator take the process down.
    const std::string blocked = model_.CheckSortFeasible();
    if (!blocked.empty()) {
      SetMessage(blocked, true);
      return true;
    }
    SortByCursorColumn(descending);
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
