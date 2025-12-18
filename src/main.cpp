
#include <ftxui/component/screen_interactive.hpp>

#include "csv_controller.h"
#include "csv_model.h"
#include "csv_view.h"
#include <iostream>

/*using namespace ftxui;*/
/**/
/*class CSVViewer : public ComponentBase,*/
/*                  public std::enable_shared_from_this<CSVViewer> {*/
/*private:*/
/*  CSVModel model;*/
/*  CSVController controller;*/
/*  CSVView view;*/
/**/
/*public:*/
/*  CSVViewer(const std::string &filename, ScreenInteractive &screen)*/
/*      : model(filename, screen), controller(model), view(model) {}*/
/**/
/*  void run() { model.screen.Loop(shared_from_this()); }*/
/**/
/*  Element Render() override { return view.render(); }*/
/**/
/*  bool OnEvent(Event event) override {*/
/*    auto updated = controller.processEvent(event);*/
/*    return updated;*/
/*  }*/
/*};*/

int main(int argc, char **argv) {
  using namespace ftxui;

  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <csv-file-path>\n";
    return 1;
  }

  std::string path = argv[1];
  CSVModel model;
  if (!model.Open(path)) {
    std::cerr << "Failed to open file: " << path << "\n";
    return 1;
  }

  CSVView view(model);
  CSVController controller(model, view);

  auto screen = ScreenInteractive::Fullscreen();
  // Hide the terminal cursor; otherwise it blinks at bottom-right.
  screen.SetCursor(Screen::Cursor{0, 0, Screen::Cursor::Hidden});
  screen.Loop(controller.GetComponent());
  // Restore cursor visibility after exiting.
  std::cout << "\033[?25h" << std::flush;

  return 0;
}
