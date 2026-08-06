#pragma once

#include <ftxui/component/component.hpp>
#include <optional>
#include <string>

#include "csv_index.h"

namespace ftxui {
class ScreenInteractive;
}

class CSVModel;
class CSVView;

class CSVController {
public:
  CSVController(CSVModel &model, CSVView &view, ftxui::ScreenInteractive &screen);

  ftxui::Component GetComponent();

private:
  enum class InputMode { Normal, Search, Filter };

  CSVModel &model_;
  CSVView &view_;
  ftxui::ScreenInteractive &screen_;
  ftxui::Component component_;

  size_t cursor_row_ = 0;
  size_t cursor_col_ = 0;
  size_t start_row_ = 0;

  size_t pending_count_ = 0;
  bool awaiting_second_g_ = false;

  InputMode input_mode_ = InputMode::Normal;
  std::string input_buffer_;
  std::optional<std::string> last_search_;

  // What to do once the background row count finishes.
  enum class PendingJump { None, End, Row, Sort, SortDesc, Filter, Stats };
  CSVIndexer indexer_;
  PendingJump pending_jump_ = PendingJump::None;
  size_t pending_row_ = 0;
  std::string pending_filter_;

  bool OnEvent(ftxui::Event event);
  bool OnOverlayEvent(ftxui::Event event);
  bool OnTextInputEvent(ftxui::Event event);
  bool OnNormalEvent(ftxui::Event event);
  bool OnMouseEvent(ftxui::Event event);

  int VisibleRows() const;
  int TerminalWidth() const;

  size_t ConsumeCount();
  bool RowExists(size_t row);
  size_t KnownLastRow();
  // Last existing row in [low, high], found with probes instead of a scan.
  size_t LastRowBetween(size_t low, size_t high);

  // Starts (or reuses) the background scan, remembering what to do after.
  void RequestExactCount(PendingJump jump, size_t row = 0,
                         const std::string &filter = std::string());
  void PollIndexer();
  bool CancelIndexing();

  void MoveCursorRows(long long delta);
  void MoveCursorColumns(long long delta);
  void GoToRow(size_t row);
  void SyncView();
  void ClampToView();
  void SetMessage(const std::string &message, bool is_error = false);
  void UpdateCommandLine();

  void RunSearch(const std::string &pattern, bool forward, bool from_cursor);
  void RepeatSearch(bool forward);
  void ApplyFilter(const std::string &pattern);
  void SortByCursorColumn(bool descending);
  void ClearOrdering();
  void YankCurrentCell();
  void ShowColumnStats();
  std::string CurrentCellValue();
};
