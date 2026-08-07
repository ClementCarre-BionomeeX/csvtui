#pragma once

#include <atomic>
#include <functional>
#include <vector>
#include <ftxui/component/component.hpp>
#include <optional>
#include <string>
#include <thread>

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
  ~CSVController();

  ftxui::Component GetComponent();

private:
  enum class InputMode { Normal, Search, Filter, Export };

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
  enum class Task { None, End, Row, Sort, Filter, Stats, MatchCount };
  CSVScanner scanner_;
  Task task_ = Task::None;
  size_t task_row_ = 0;               // target row, for Task::Row
  size_t task_column_ = 0;            // subject column, for Task::Stats
  CSVModel::ViewState task_view_;     // the view the pass is building
  std::string task_label_;            // "sorting by price", shown while it runs
  bool cancel_requested_ = false;     // Esc pressed; the worker is winding down
  size_t spinner_frame_ = 0;          // advances every frame a pass is running

  // Work that has to read rows through the model rather than through its own
  // file handle — searching, and writing the view out. CSVModel is not
  // thread-safe and this does not make it so: while one of these runs the
  // interface takes no key but Esc and draws the frame it had when the work
  // began, which is what guarantees nothing on the UI thread reads the model
  // while the worker has it.
  //
  // For a search that restriction is also the sensible behaviour, since a
  // search produces a cursor position and moving the cursor while something
  // looks for where to put it means nothing.
  enum class Blocking { None, Search, Export };
  Blocking blocking_ = Blocking::None;
  std::thread blocking_thread_;
  std::atomic<bool> blocking_cancel_{false};
  std::atomic<bool> blocking_finished_{false};
  std::atomic<size_t> blocking_progress_{0};
  std::string blocking_label_;
  std::string blocking_message_;
  ftxui::Element frozen_frame_;

  std::string search_pattern_;
  bool search_forward_ = true;
  size_t search_origin_row_ = 0;
  size_t search_origin_col_ = 0;
  std::optional<CSVModel::SearchHit> search_hit_;

  std::string export_path_;
  std::string export_error_;

  // Counting every match needs a whole pass, which is why search never used to
  // say "of 17". It runs on its own file handle after a hit is found, so it
  // does not block anything and the total simply arrives late.
  std::string match_pattern_;
  size_t match_total_ = 0;
  bool match_total_known_ = false;

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
  void ApplySearchHit(const std::string &pattern, bool forward, size_t row,
                      size_t col,
                      const std::optional<CSVModel::SearchHit> &hit);
  void StartSearch(const std::string &pattern, bool forward, bool from_cursor);
  void StartExport(const std::string &path);
  // Runs on the worker. Returns an empty string on success, otherwise why not.
  std::string WriteViewTo(const std::string &path,
                          const std::vector<size_t> &columns,
                          const std::function<void(size_t)> &report);

  // Runs `body` on a worker with the interface frozen to Esc.
  void StartBlocking(Blocking kind, const std::string &label,
                     std::function<void()> body);
  void PollBlocking();
  bool CancelBlocking();
  void JoinBlocking();
  bool BlockingCancelled() const;
  // A progress callback that stores every value but only wakes the UI ten
  // times a second, since the work offers ticks far faster than that.
  std::function<void(size_t)> BlockingReporter();
  ftxui::Element RenderWhileBlocked();
  void RepeatSearch(bool forward);
  void ApplyFilter(const std::string &pattern);
  void SortByCursorColumn(bool descending);
  void ClearOrdering();
  void YankCurrentCell();
  void StartMatchCount(const std::string &pattern);
  void ShowColumnStats();
  std::string DescribeStats(size_t col, const csvscan::Stats &stats) const;
  std::string CurrentCellValue();
};
