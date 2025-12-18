
#include "csv_controller.h"
#include "csv_model.h"
#include "csv_view.h"

#include <cctype>
#include <optional>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

using namespace ftxui;

CSVController::CSVController(CSVModel &model, CSVView &view)
    : model_(model), view_(view) {
  UpdateViewport();
  view_.SetCommandLine(command_buffer_, last_command_);

  container_ = CatchEvent(Renderer([this] { return view_.Render()->Render(); }),
                          [&](Event event) {
                            if (search_mode_) {
                              if (event == Event::Return) {
                                if (!search_buffer_.empty()) {
                                  auto match = model_.FindNext(search_buffer_, start_row_);
                                  if (match) {
                                    start_row_ = match->row;
                                    UpdateViewport();
                                    last_search_ = search_buffer_;
                                    last_command_ = "/" + search_buffer_;
                                    view_.SetSearchPattern(search_buffer_);
                                    SetCurrentMatch(match->row, match->col);
                                  } else {
                                    last_command_ = "/" + search_buffer_ + " (not found)";
                                    ClearCurrentMatch();
                                  }
                                }
                                search_mode_ = false;
                                command_buffer_.clear();
                                view_.SetCommandLine(command_buffer_, last_command_);
                                return true;
                              }
                              if (event == Event::Escape) {
                                search_mode_ = false;
                                search_buffer_.clear();
                                command_buffer_.clear();
                                view_.SetCommandLine(command_buffer_, last_command_);
                                ClearCurrentMatch();
                                return true;
                              }
                              if (event == Event::Backspace) {
                                if (!search_buffer_.empty()) {
                                  search_buffer_.pop_back();
                                  command_buffer_.pop_back();
                                  view_.SetCommandLine(command_buffer_, last_command_);
                                }
                                return true;
                              }
                              if (event.is_character()) {
                                char ch = event.character()[0];
                                search_buffer_.push_back(ch);
                                command_buffer_.push_back(ch);
                                view_.SetCommandLine(command_buffer_, last_command_);
                                return true;
                              }
                              return false;
                            }

                            // Accumulate numeric prefix for vim-like motions.
                            if (event.is_character()) {
                              const auto ch = event.character()[0];
                              if (std::isdigit(ch)) {
                                pending_count_ = pending_count_ * 10 + (ch - '0');
                                awaiting_second_g_ = false;
                                command_buffer_.push_back(ch);
                                view_.SetCommandLine(command_buffer_, last_command_);
                                return true;
                              } else if (ch == '/') {
                                search_mode_ = true;
                                search_buffer_.clear();
                                command_buffer_ = "/";
                                view_.SetCommandLine(command_buffer_, last_command_);
                                return true;
                              }
                            }

                            const auto consume_count = [this]() {
                              size_t count = pending_count_ == 0 ? 1 : pending_count_;
                              pending_count_ = 0;
                              awaiting_second_g_ = false;
                              return count;
                            };

                            if (event == Event::Character('q')) {
                              exit(0);
                            } else if (event == Event::Character('g')) {
                              if (awaiting_second_g_) {
                                size_t target = pending_count_ == 0 ? 1 : pending_count_;
                                GoToLine(target);
                                last_command_ = command_buffer_.empty()
                                                    ? "gg"
                                                    : command_buffer_ + "g";
                                command_buffer_.clear();
                                pending_count_ = 0;
                                awaiting_second_g_ = false;
                              } else {
                                awaiting_second_g_ = true;
                                command_buffer_.push_back('g');
                              }
                              view_.SetCommandLine(command_buffer_, last_command_);
                              return true;
                            } else if (event == Event::Character('G')) {
                              size_t target = pending_count_ == 0 ? model_.RowCount()
                                                                   : pending_count_;
                              GoToLine(target == 0 ? 1 : target);
                              last_command_ =
                                  (pending_count_ == 0
                                       ? std::string("G")
                                       : std::to_string(pending_count_) + "G");
                              pending_count_ = 0;
                              awaiting_second_g_ = false;
                              command_buffer_.clear();
                              view_.SetCommandLine(command_buffer_, last_command_);
                              return true;
                            } else if (event == Event::ArrowRight ||
                                       event == Event::Character('l')) {
                              size_t count = consume_count();
                              view_.AdjustColumnOffset(static_cast<int>(count));
                              last_command_ =
                                  (count == 1) ? "l" : std::to_string(count) + "l";
                              command_buffer_.clear();
                              view_.SetCommandLine(command_buffer_, last_command_);
                              return true;
                            } else if (event == Event::ArrowLeft ||
                                       event == Event::Character('h')) {
                              size_t count = consume_count();
                              view_.AdjustColumnOffset(-static_cast<int>(count));
                              last_command_ =
                                  (count == 1) ? "h" : std::to_string(count) + "h";
                              command_buffer_.clear();
                              view_.SetCommandLine(command_buffer_, last_command_);
                              return true;
                            } else if (event == Event::Character('j') ||
                                       event == Event::ArrowDown) {
                              size_t count = consume_count();
                              MoveRows(static_cast<int>(count));
                              UpdateViewport();
                              last_command_ =
                                  (count == 1) ? "j" : std::to_string(count) + "j";
                              command_buffer_.clear();
                              view_.SetCommandLine(command_buffer_, last_command_);
                              return true;
                            } else if (event == Event::Character('k') ||
                                       event == Event::ArrowUp) {
                              size_t count = consume_count();
                              MoveRows(-static_cast<int>(count));
                              UpdateViewport();
                              last_command_ =
                                  (count == 1) ? "k" : std::to_string(count) + "k";
                              command_buffer_.clear();
                              view_.SetCommandLine(command_buffer_, last_command_);
                              return true;
                            } else if (event == Event::Character('H')) {
                              view_.ToggleHeaderPinned();
                              last_command_ = "H";
                              pending_count_ = 0;
                              awaiting_second_g_ = false;
                              command_buffer_.clear();
                              view_.SetCommandLine(command_buffer_, last_command_);
                              return true;
                            } else if (event == Event::Character('t')) {
                              view_.ToggleTabularMode();
                              last_command_ = "t";
                              pending_count_ = 0;
                              awaiting_second_g_ = false;
                              command_buffer_.clear();
                              view_.SetCommandLine(command_buffer_, last_command_);
                              return true;
                            } else if (event == Event::Character('n')) {
                              if (last_search_) {
                                size_t start = start_row_ + 1;
                                auto match = model_.FindNext(*last_search_, start);
                                if (match) {
                                  start_row_ = match->row;
                                  UpdateViewport();
                                  last_command_ = "n";
                                  view_.SetSearchPattern(*last_search_);
                                  SetCurrentMatch(match->row, match->col);
                                } else {
                                  last_command_ = "n (not found)";
                                  ClearCurrentMatch();
                                }
                                command_buffer_.clear();
                                view_.SetCommandLine(command_buffer_, last_command_);
                                return true;
                              }
                            } else if (event == Event::Character('N')) {
                              if (last_search_) {
                                size_t start = start_row_ == 0 ? 0 : start_row_ - 1;
                                auto match = model_.FindPrev(*last_search_, start);
                                if (match) {
                                  start_row_ = match->row;
                                  UpdateViewport();
                                  last_command_ = "N";
                                  view_.SetSearchPattern(*last_search_);
                                  SetCurrentMatch(match->row, match->col);
                                } else {
                                  last_command_ = "N (not found)";
                                  ClearCurrentMatch();
                                }
                                command_buffer_.clear();
                                view_.SetCommandLine(command_buffer_, last_command_);
                                return true;
                              }
                            }
                            awaiting_second_g_ = false;
                            pending_count_ = 0;
                            command_buffer_.clear();
                            view_.SetCommandLine(command_buffer_, last_command_);
                            return false;
                          });
}

ftxui::Component CSVController::GetComponent() { return container_; }

void CSVController::UpdateViewport() {
  model_.SetViewport(start_row_, visible_rows_);
  view_.SetStartRow(start_row_);
}

void CSVController::MoveRows(int delta) {
  const bool count_known = model_.RowCountKnown();
  const size_t row_count = count_known ? model_.RowCount() : 0;
  if (delta == 0)
    return;
  if (delta > 0) {
    size_t max_start =
        count_known && row_count > visible_rows_ ? row_count - visible_rows_ : row_count;
    size_t next = start_row_ + static_cast<size_t>(delta);
    if (count_known && next > max_start)
      next = max_start;
    start_row_ = next;
  } else {
    size_t abs_delta = static_cast<size_t>(-delta);
    if (abs_delta > start_row_)
      start_row_ = 0;
    else
      start_row_ -= abs_delta;
  }
}

void CSVController::GoToLine(size_t target) {
  // target is 1-based from the user's perspective.
  if (target == 0)
    target = 1;
  size_t row_index = target - 1;
  size_t row_count = model_.RowCount();
  if (row_index >= row_count)
    row_index = row_count == 0 ? 0 : row_count - 1;

  // Align viewport so that target row is at the top if possible.
  start_row_ = row_index;
  UpdateViewport();
}

void CSVController::SetCurrentMatch(std::optional<size_t> row,
                                    std::optional<size_t> col) {
  current_match_row_ = row;
  current_match_col_ = col;
  view_.SetCurrentMatch(row, col);
}

void CSVController::ClearCurrentMatch() {
  current_match_row_.reset();
  current_match_col_.reset();
  view_.SetCurrentMatch(std::nullopt, std::nullopt);
}
