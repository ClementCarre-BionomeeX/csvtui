#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>

#include "csv_controller.h"
#include "csv_model.h"
#include "csv_view.h"

namespace {

constexpr const char *kVersion = "0.2.0";

void PrintUsage(std::ostream &out, const char *program) {
  out << "csvtui " << kVersion << " — a terminal viewer for CSV files\n\n"
      << "Usage:\n"
      << "  " << program << " [options] <file.csv>\n"
      << "  <command> | " << program << " [options] -\n\n"
      << "Options:\n"
      << "  -d, --delimiter <char>  Field delimiter (default: auto-detect).\n"
      << "                          Use 'tab' or '\\t' for tab-separated files.\n"
      << "      --no-header         Treat the first row as data, not a header.\n"
      << "      --header            Force the first row to be a header (default).\n"
      << "  -h, --help              Show this help and exit.\n"
      << "  -V, --version           Show the version and exit.\n\n"
      << "Keys (press ? inside the viewer for the full list):\n"
      << "  h j k l   move        / n N   search        s S   sort\n"
      << "  gg G      first/last  f       filter        y     copy cell\n"
      << "  Enter     cell detail ?       help          q     quit\n";
}

// Returns false when the delimiter spelling is not understood.
bool ParseDelimiter(const std::string &text, char &out) {
  if (text == "tab" || text == "\\t") {
    out = '\t';
    return true;
  }
  if (text == "\\0" || text.empty())
    return false;
  if (text.size() != 1)
    return false;
  out = text[0];
  return true;
}

// True when stdin actually carries data: a pipe or a redirected file. A
// character device such as /dev/null is not input, it is the absence of it.
bool StdinHasData() {
  struct stat info {};
  if (::fstat(STDIN_FILENO, &info) != 0)
    return false;
  return S_ISFIFO(info.st_mode) || S_ISREG(info.st_mode);
}

// Copies stdin into a temporary file so the viewer keeps random access, then
// reconnects stdin to the terminal so keystrokes still reach the UI.
bool SpoolStdin(std::string &path_out, std::string &error) {
  char tmpl[] = "/tmp/csvtui-stdin-XXXXXX";
  const int fd = ::mkstemp(tmpl);
  if (fd < 0) {
    error = std::string("cannot create a temporary file: ") + std::strerror(errno);
    return false;
  }

  char buffer[65536];
  ssize_t bytes = 0;
  while ((bytes = ::read(STDIN_FILENO, buffer, sizeof(buffer))) > 0) {
    ssize_t written = 0;
    while (written < bytes) {
      const ssize_t chunk = ::write(fd, buffer + written, bytes - written);
      if (chunk <= 0) {
        ::close(fd);
        ::unlink(tmpl);
        error = "cannot write the temporary file";
        return false;
      }
      written += chunk;
    }
  }
  ::close(fd);

  if (std::freopen("/dev/tty", "r", stdin) == nullptr) {
    ::unlink(tmpl);
    error = "no terminal available to read keys from (is this a pipe on both ends?)";
    return false;
  }

  path_out = tmpl;
  return true;
}

} // namespace

int main(int argc, char **argv) {
  using namespace ftxui;

  std::optional<char> delimiter;
  std::optional<bool> has_header;
  std::string path;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      PrintUsage(std::cout, argv[0]);
      return 0;
    }
    if (arg == "-V" || arg == "--version") {
      std::cout << "csvtui " << kVersion << "\n";
      return 0;
    }
    if (arg == "--no-header") {
      has_header = false;
      continue;
    }
    if (arg == "--header") {
      has_header = true;
      continue;
    }
    if (arg == "-d" || arg == "--delimiter") {
      if (i + 1 >= argc) {
        std::cerr << "csvtui: " << arg << " needs a value\n";
        return 2;
      }
      char parsed = ',';
      if (!ParseDelimiter(argv[++i], parsed)) {
        std::cerr << "csvtui: not a valid delimiter: " << argv[i] << "\n";
        return 2;
      }
      delimiter = parsed;
      continue;
    }
    if (arg.rfind("--delimiter=", 0) == 0) {
      char parsed = ',';
      if (!ParseDelimiter(arg.substr(12), parsed)) {
        std::cerr << "csvtui: not a valid delimiter: " << arg.substr(12) << "\n";
        return 2;
      }
      delimiter = parsed;
      continue;
    }
    if (arg.size() > 1 && arg[0] == '-' && arg != "-") {
      std::cerr << "csvtui: unknown option: " << arg << "\n";
      PrintUsage(std::cerr, argv[0]);
      return 2;
    }
    if (!path.empty()) {
      std::cerr << "csvtui: only one file can be opened at a time\n";
      return 2;
    }
    path = arg;
  }

  // No path given: read from a pipe if there is one, otherwise show usage.
  const bool from_stdin = (path == "-") || (path.empty() && StdinHasData());
  std::string spooled;
  if (from_stdin) {
    std::string error;
    if (!SpoolStdin(spooled, error)) {
      std::cerr << "csvtui: " << error << "\n";
      return 1;
    }
    path = spooled;
  } else if (path.empty()) {
    PrintUsage(std::cerr, argv[0]);
    return 2;
  }

  CSVModel model;
  const std::string error = model.Open(path, delimiter, has_header);
  if (!error.empty()) {
    std::cerr << "csvtui: " << error << "\n";
    if (!spooled.empty())
      ::unlink(spooled.c_str());
    return 1;
  }

  if (!spooled.empty())
    model.SetDisplayName("(stdin)");

  {
    CSVView view(model);
    auto screen = ScreenInteractive::Fullscreen();
    screen.TrackMouse(true);
    CSVController controller(model, view, screen);
    // Loop() restores the terminal on the way out, which is exactly why the
    // controller calls screen.Exit() instead of ::exit().
    screen.Loop(controller.GetComponent());
  }

  if (!spooled.empty())
    ::unlink(spooled.c_str());
  return 0;
}
