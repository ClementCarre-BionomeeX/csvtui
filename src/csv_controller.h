#pragma once

#include <ftxui/component/component.hpp>
#include <optional>
#include <string>

#include "csv_model.h"
#include "csv_scan.h"

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

  // Work that needs a full pass over the file. It runs on a worker thread, so
  // the UI stays live and Esc cancels; `task_` says what to do with the result.
  enum class Task { None, End, Row, Sort, Filter, Stats };
  CSVScanner scanner_;
  Task task_ = Task::None;
  size_t task_row_ = 0;               // target row, for Task::Row
  size_t task_column_ = 0;            // subject column, for Task::Stats
  CSVModel::ViewState task_view_;     // the view the pass is building
  std::string task_label_;            // "sorting by price", shown while it runs
  bool cancel_requested_ = false;     // Esc pressed; the worker is winding down
  size_t spinner_frame_ = 0;          // advances every frame a pass is running

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

  // Starts a background pass, replacing any pass already in flight.
  void StartScan(Task task, const csvscan::Request &request,
                 const std::string &label);
  // Starts a pass that only counts rows, remembering what to do afterwards.
  void RequestExactCount(Task task, size_t row = 0);
  void PollScanner();
  bool CancelScan();
  void FinishScan(csvscan::Result &result);

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
  std::string DescribeStats(size_t col, const csvscan::Stats &stats) const;
  std::string CurrentCellValue();
};
