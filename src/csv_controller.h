
#pragma once

#include <ftxui/component/component.hpp>
#include <optional>
#include <memory>

class CSVModel;
class CSVView;

class CSVController {
public:
  CSVController(CSVModel &model, CSVView &view);

  ftxui::Component GetComponent();

private:
  CSVModel &model_;
  CSVView &view_;
  ftxui::Component container_;

  // Viewport
  size_t start_row_ = 0;
  const size_t visible_rows_ = 30; // You can tune this based on terminal size

  size_t pending_count_ = 0;
  bool awaiting_second_g_ = false;
  std::string command_buffer_;
  std::string last_command_;
  bool search_mode_ = false;
  std::string search_buffer_;
  std::optional<std::string> last_search_;

  void UpdateViewport();
  void MoveRows(int delta);
  void GoToLine(size_t target);
};
