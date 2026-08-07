#include "test_util.h"

#include "csv_model.h"
#include "csv_view.h"

#include "csv_parser.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

// Snapshots of the rendered screen.
//
// Every other test in this suite covers the parser and the model. The half the
// user actually looks at had none, and the two layout bugs that got furthest —
// a status bar that overflowed a narrow terminal, and messages truncated to
// nothing — were caught by squinting at screenshots, which does not scale.
//
// These render the real view off-screen and compare the characters against a
// file checked in beside them. Colour is deliberately dropped: it is the thing
// most likely to change for good reasons, and the layout is what breaks.
//
// When a change is intended, regenerate and read the diff before committing:
//
//     CSVTUI_UPDATE_GOLDEN=1 ./build/csvtui_tests
//     git diff tests/golden
namespace {

std::string GoldenPath(const std::string &name) {
  return std::string(CSVTUI_TEST_DIR) + "/golden/" + name + ".txt";
}

// The characters on screen, with trailing blanks trimmed so the files stay
// readable and an editor stripping whitespace cannot break the suite.
std::string ScreenText(const ftxui::Element &element, int width, int height) {
  ftxui::Screen screen(width, height);
  ftxui::Element copy = element;
  copy->ComputeRequirement();
  copy->SetBox({0, width - 1, 0, height - 1});
  copy->Render(screen);

  std::string out;
  for (int y = 0; y < height; ++y) {
    std::string line;
    for (int x = 0; x < width; ++x)
      line += screen.PixelAt(x, y).character;
    while (!line.empty() && line.back() == ' ')
      line.pop_back();
    out += line;
    out += '\n';
  }
  return out;
}

bool ShouldUpdate() {
  const char *value = std::getenv("CSVTUI_UPDATE_GOLDEN");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

void CheckAgainstGolden(const std::string &name, const std::string &actual) {
  const std::string path = GoldenPath(name);

  if (ShouldUpdate()) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << actual;
    return;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    ++Failures();
    std::cerr << "    FAIL missing golden file " << path
              << "\n      rerun with CSVTUI_UPDATE_GOLDEN=1 to create it\n";
    return;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const std::string expected = buffer.str();

  if (expected == actual)
    return;

  const auto split = [](const std::string &text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
      lines.push_back(line);
    return lines;
  };
  const std::vector<std::string> want = split(expected);
  const std::vector<std::string> got = split(actual);

  ++Failures();
  std::cerr << "    FAIL screen does not match " << path << "\n";
  for (size_t i = 0; i < std::max(want.size(), got.size()); ++i) {
    const std::string want_line = i < want.size() ? want[i] : "<missing>";
    const std::string got_line = i < got.size() ? got[i] : "<missing>";
    if (want_line != got_line) {
      std::cerr << "      line " << (i + 1) << "\n        expected: |"
                << want_line << "|\n        actual:   |" << got_line << "|\n";
    }
  }
  std::cerr << "      rerun with CSVTUI_UPDATE_GOLDEN=1 if this is intended\n";
}

// A fixed file: accented and double-width text so column alignment is
// measured in terminal cells, a quoted delimiter, an empty cell, and a numeric
// column that should sit to the right of its field.
const char *const kSample =
    "id,name,city,role,score\n"
    "1,Alice,Genève,Admin,7\n"
    "2,Bob,Zürich,Editor,42\n"
    "3,\"Dupont, Jean\",Paris,,1000\n"
    "4,中文字,Lyon,Viewer,8\n"
    "5,Eve,Besançon,Admin,\n"
    "6,Frank,Neuchâtel,Editor,3\n";

// Wraps the pieces a render needs, with a stable name in the status bar: the
// real file is a temporary whose name changes every run.
class Fixture {
public:
  explicit Fixture(const char *contents = kSample) : file_(contents) {
    CHECK_EQ(model_.Open(file_.path(), {}, {}), std::string(""));
    model_.SetDisplayName("sample.csv");
    view_.reset(new CSVView(model_));
  }

  CSVModel &model() { return model_; }
  CSVView &view() { return *view_; }

  std::string Render(int width, int height) {
    return ScreenText(view_->Render(width, height), width, height);
  }

private:
  TempCSV file_;
  CSVModel model_;
  std::unique_ptr<CSVView> view_;
};

} // namespace

TEST(ViewRendersTheGrid) {
  Fixture fixture;
  fixture.view().SetCursor(0, 0);
  CheckAgainstGolden("grid-80x14", fixture.Render(80, 14));
}

TEST(ViewRendersWithTheCursorInsideTheTable) {
  Fixture fixture;
  fixture.view().SetCursor(3, 2);
  fixture.view().EnsureColumnVisible(2, 80);
  CheckAgainstGolden("cursor-80x14", fixture.Render(80, 14));
}

// The status bar has to drop segments rather than overflow, and it has grown
// progress readouts and a cancel hint since that was last true.
TEST(ViewFitsANarrowTerminal) {
  Fixture fixture;
  fixture.view().SetCursor(1, 1);
  CheckAgainstGolden("narrow-40x12", fixture.Render(40, 12));
}

TEST(ViewFitsAVeryNarrowTerminal) {
  Fixture fixture;
  fixture.view().SetCursor(1, 1);
  CheckAgainstGolden("narrow-24x10", fixture.Render(24, 10));
}

TEST(ViewRendersALongMessageWithoutLosingTheGrid) {
  Fixture fixture;
  fixture.view().SetCursor(0, 0);
  // The shape of a refusal, which is the longest thing the status line shows.
  fixture.view().SetStatusMessage(
      "sorting ~156 000 000 rows needs ~9.3 GB, only 4.2 GB usable — filter "
      "first",
      true);
  CheckAgainstGolden("long-message-80x14", fixture.Render(80, 14));
}

TEST(ViewRendersProgressWhileAPassRuns) {
  Fixture fixture;
  fixture.view().SetCursor(0, 0);
  fixture.view().SetStatusMessage(
      "sorting by score… 43%  (10 200 000 rows, Esc to cancel)", false);
  CheckAgainstGolden("progress-80x14", fixture.Render(80, 14));
}

TEST(ViewRendersTheHelpOverlay) {
  Fixture fixture;
  fixture.view().ToggleHelp();
  CHECK(fixture.view().HelpVisible());
  CheckAgainstGolden("help-80x24", fixture.Render(80, 24));
}

TEST(ViewRendersTheCellDetail) {
  Fixture fixture;
  fixture.view().SetCursor(2, 1); // the quoted "Dupont, Jean"
  fixture.view().ToggleCellDetail();
  CheckAgainstGolden("cell-detail-80x14", fixture.Render(80, 14));
}

TEST(ViewRendersASearchPrompt) {
  Fixture fixture;
  fixture.view().SetCursor(0, 0);
  fixture.view().SetCommandLine("/Gen");
  CheckAgainstGolden("prompt-80x14", fixture.Render(80, 14));
}

TEST(ViewMarksHiddenAndFrozenColumns) {
  Fixture fixture;
  fixture.view().ToggleColumnHidden(2); // hide city
  fixture.view().SetFrozenColumns(1);
  fixture.view().SetCursor(1, 3);
  CheckAgainstGolden("hidden-frozen-80x14", fixture.Render(80, 14));
}

TEST(ViewRendersRawMode) {
  Fixture fixture;
  fixture.view().ToggleTabularMode();
  CHECK(!fixture.view().TabularMode());
  CheckAgainstGolden("raw-80x14", fixture.Render(80, 14));
}

TEST(ViewRendersASortedAndFilteredStatus) {
  Fixture fixture;
  fixture.model().ApplyFilter("Admin");
  fixture.model().SortByColumn(4, true);
  fixture.view().SetCursor(0, 4);
  CheckAgainstGolden("sorted-filtered-80x14", fixture.Render(80, 14));
}

// Not a golden: a property. Whatever the contents, nothing may spill past the
// right-hand edge, because a line that does corrupts the terminal.
TEST(ViewNeverExceedsTheTerminalWidth) {
  Fixture fixture;
  fixture.view().SetStatusMessage(std::string(400, 'x'), true);
  fixture.view().SetCommandLine("/" + std::string(400, 'y'));

  for (int width : {16, 24, 40, 61, 80, 120, 200}) {
    const std::string text = fixture.Render(width, 12);
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
      CHECK(csv::DisplayWidth(line) <= width);
    }
  }
}

TEST(ViewSurvivesATerminalTooSmallToUse) {
  Fixture fixture;
  // One column of one row: it must render something rather than misbehave.
  for (int width : {1, 2, 4}) {
    for (int height : {1, 2, 3}) {
      const std::string text = fixture.Render(width, height);
      CHECK(!text.empty());
    }
  }
}
