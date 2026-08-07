#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Sorting a file larger than memory.
//
// Holding one key per row is fine until the row count gets large: at roughly
// 56 bytes a key, a 12 GB export wants about 9 GB just for the keys, which is
// why sorting one used to be refused outright. The classic answer applies —
// fill a bounded buffer, sort it, write it out as a "run", and merge the runs
// back together at the end — and it turns the cost of a sort from one
// proportional to the file into one you choose in advance.
//
// What stays in memory is the answer itself: one row number per row, eight
// bytes. That is a sixth of what a key costs, and it is what the model needs
// in order to show you row 4 000 000 without reading everything before it.
namespace csvsort {

// One row's sort key. The column value is extracted once so that comparison
// never touches the CSV, and the row number rides along so the permutation
// falls out of the sort itself rather than needing a second index array.
struct Key {
  std::string text;
  double number = 0.0;
  size_t row = 0;
  bool numeric = false;
};

// Ordering keys directly, rather than sorting an index that points at them.
//
// The row number is the final tiebreak, which makes this a total order and so
// makes plain std::sort produce exactly what std::stable_sort would. That
// matters for more than tidiness: stable_sort asks for a temporary buffer as
// large as the data, and on a file with tens of millions of rows that buffer
// doubles the peak memory of a sort.
//
// Numbers sort before text in both directions, so empty and non-numeric cells
// collect at the end whichever way you sort. Ties keep file order either way.
struct Order {
  bool descending = false;

  bool operator()(const Key &a, const Key &b) const {
    if (a.numeric != b.numeric)
      return a.numeric;
    if (a.numeric) {
      if (a.number != b.number)
        return descending ? b.number < a.number : a.number < b.number;
    } else if (a.text != b.text) {
      return descending ? b.text < a.text : a.text < b.text;
    }
    return a.row < b.row;
  }
};

// Approximate footprint of one key, used to decide when the buffer is full.
size_t KeyBytes(const Key &key);

// Where spill files go: $CSVTUI_TMPDIR, else $TMPDIR, else /tmp. Worth
// overriding when /tmp is a small tmpfs and the file being sorted is not.
std::string TempDirectory();

// Holds the sorted runs belonging to one sort, and merges them back.
//
// Every file it creates is deleted when it goes out of scope, including when a
// sort is cancelled half way through — a viewer that leaves gigabytes behind
// in /tmp after you press Esc would not be worth the memory it saved.
class RunStore {
public:
  explicit RunStore(std::string directory);
  ~RunStore();

  RunStore(const RunStore &) = delete;
  RunStore &operator=(const RunStore &) = delete;

  bool empty() const { return paths_.empty(); }
  size_t run_count() const { return paths_.size(); }
  // Empty unless something went wrong; a sort that cannot spill says why.
  const std::string &error() const { return error_; }

  // Sorts `keys` and writes them out as one run, leaving `keys` empty but
  // keeping its capacity so the next batch reuses the same buffer.
  bool Spill(std::vector<Key> &keys, const Order &order);

  // Merges every run, plus `tail` (the unspilled remainder, sorted here),
  // appending row numbers to `out` in order. `cancelled` is polled every few
  // thousand rows and may be empty.
  bool Merge(std::vector<Key> &tail, const Order &order,
             std::vector<size_t> &out, const std::function<bool()> &cancelled);

private:
  std::string directory_;
  std::vector<std::string> paths_;
  std::string error_;
};

} // namespace csvsort
