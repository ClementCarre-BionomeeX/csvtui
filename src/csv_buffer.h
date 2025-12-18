#pragma once
#include <string>
#include <vector>

class CSVBuffer {

private:
  std::vector<std::string> buffer;
  size_t current_chunk = 0;
  size_t chunk_size = 32;
  std::string filename;
  size_t current_line = 0;

public:
  CSVBuffer(std::string filename);
  void reload() {
    auto chunk = which_chunk(current_line);
    if (chunk == current_chunk) {
      return;
    }
    // open file, read chunk_size * 3 from chunk - 1 (max 0)
    // go to i
  }
  size_t which_chunk(size_t line);
};
