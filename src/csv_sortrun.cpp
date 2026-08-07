#include "csv_sortrun.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <queue>
#include <unistd.h>

namespace csvsort {
namespace {

// Poll for cancellation this often while merging. Frequent enough to feel
// immediate, rare enough not to matter.
constexpr size_t kCancelCheckRows = 4096;

// Read-ahead per run. With a few dozen runs this is a megabyte or two in
// total, which is noise next to the buffer the spilling was there to bound.
constexpr size_t kRunBufferBytes = 64 * 1024;

void WritePod(std::ostream &out, const void *data, size_t size) {
  out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
}

bool WriteKey(std::ostream &out, const Key &key) {
  const std::uint8_t numeric = key.numeric ? 1 : 0;
  const std::uint64_t row = key.row;
  const std::uint32_t length = static_cast<std::uint32_t>(key.text.size());
  WritePod(out, &numeric, sizeof(numeric));
  WritePod(out, &key.number, sizeof(key.number));
  WritePod(out, &row, sizeof(row));
  WritePod(out, &length, sizeof(length));
  if (length != 0)
    WritePod(out, key.text.data(), length);
  return static_cast<bool>(out);
}

// Reads one run back, one key at a time. The file was written by this process
// moments ago and is deleted when the sort ends, so the layout is whatever the
// machine's own representation happens to be — it never has to be portable.
class RunReader {
public:
  explicit RunReader(const std::string &path)
      : file_(path, std::ios::binary), buffer_(kRunBufferBytes) {
    if (file_.is_open())
      file_.rdbuf()->pubsetbuf(buffer_.data(), kRunBufferBytes);
  }

  bool ok() const { return file_.is_open(); }

  bool Next(Key &key) {
    std::uint8_t numeric = 0;
    if (!file_.read(reinterpret_cast<char *>(&numeric), sizeof(numeric)))
      return false;
    std::uint64_t row = 0;
    std::uint32_t length = 0;
    if (!file_.read(reinterpret_cast<char *>(&key.number), sizeof(key.number)))
      return false;
    if (!file_.read(reinterpret_cast<char *>(&row), sizeof(row)))
      return false;
    if (!file_.read(reinterpret_cast<char *>(&length), sizeof(length)))
      return false;

    key.numeric = numeric != 0;
    key.row = static_cast<size_t>(row);
    key.text.resize(length);
    if (length != 0 && !file_.read(&key.text[0], length))
      return false;
    return true;
  }

private:
  std::ifstream file_;
  std::vector<char> buffer_;
};

} // namespace

size_t KeyBytes(const Key &key) {
  // The struct itself, plus whatever the string had to put on the heap. Short
  // values live inside the string object, which is most of them.
  const size_t inline_capacity = sizeof(std::string);
  const size_t heap = key.text.size() >= inline_capacity ? key.text.size() : 0;
  return sizeof(Key) + heap;
}

std::string TempDirectory() {
  for (const char *name : {"CSVTUI_TMPDIR", "TMPDIR"}) {
    const char *value = std::getenv(name);
    if (value != nullptr && value[0] != '\0') {
      std::string path(value);
      while (path.size() > 1 && path.back() == '/')
        path.pop_back();
      return path;
    }
  }
  return "/tmp";
}

RunStore::RunStore(std::string directory) : directory_(std::move(directory)) {
  if (directory_.empty())
    directory_ = ".";
}

RunStore::~RunStore() {
  for (const std::string &path : paths_)
    ::unlink(path.c_str());
}

bool RunStore::Spill(std::vector<Key> &keys, const Order &order) {
  if (keys.empty())
    return true;

  std::string pattern = directory_ + "/csvtui-sort-XXXXXX";
  std::vector<char> name(pattern.begin(), pattern.end());
  name.push_back('\0');

  const int fd = ::mkstemp(name.data());
  if (fd < 0) {
    error_ = std::string("cannot write sort run to ") + directory_ + ": " +
             std::strerror(errno);
    return false;
  }
  ::close(fd);

  const std::string path(name.data());
  // Recorded before writing: if the write fails part way through, the file
  // still exists and still has to be cleaned up.
  paths_.push_back(path);

  std::sort(keys.begin(), keys.end(), order);

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    error_ = "cannot open sort run " + path;
    return false;
  }
  std::vector<char> buffer(kRunBufferBytes);
  out.rdbuf()->pubsetbuf(buffer.data(), kRunBufferBytes);

  for (const Key &key : keys) {
    if (!WriteKey(out, key)) {
      error_ = "sort run write failed, probably out of disk space in " +
               directory_;
      return false;
    }
  }
  out.flush();
  if (!out) {
    error_ = "sort run write failed, probably out of disk space in " +
             directory_;
    return false;
  }

  keys.clear(); // keeps the capacity, so the next batch reuses the buffer
  return true;
}

bool RunStore::Merge(std::vector<Key> &tail, const Order &order,
                     std::vector<size_t> &out,
                     const std::function<bool()> &cancelled) {
  std::sort(tail.begin(), tail.end(), order);

  std::vector<std::unique_ptr<RunReader>> readers;
  readers.reserve(paths_.size());
  for (const std::string &path : paths_) {
    auto reader = std::unique_ptr<RunReader>(new RunReader(path));
    if (!reader->ok()) {
      error_ = "cannot reopen sort run " + path;
      return false;
    }
    readers.push_back(std::move(reader));
  }

  // One entry per source: the runs, then the in-memory tail as the last one.
  struct Head {
    Key key;
    size_t source = 0;
  };
  const size_t tail_source = readers.size();

  // std::priority_queue is a max-heap, so the comparator is inverted to make
  // the smallest key surface first.
  const auto greater = [&order](const Head &a, const Head &b) {
    return order(b.key, a.key);
  };
  std::priority_queue<Head, std::vector<Head>, decltype(greater)> heap(greater);

  for (size_t i = 0; i < readers.size(); ++i) {
    Head head;
    head.source = i;
    if (readers[i]->Next(head.key))
      heap.push(std::move(head));
  }

  size_t tail_index = 0;
  if (tail_index < tail.size()) {
    Head head;
    head.source = tail_source;
    head.key = tail[tail_index++];
    heap.push(std::move(head));
  }

  size_t since_check = 0;
  while (!heap.empty()) {
    if (++since_check >= kCancelCheckRows) {
      since_check = 0;
      if (cancelled && cancelled())
        return false;
    }

    Head head = heap.top();
    heap.pop();
    out.push_back(head.key.row);

    if (head.source == tail_source) {
      if (tail_index < tail.size()) {
        head.key = tail[tail_index++];
        heap.push(std::move(head));
      }
    } else if (readers[head.source]->Next(head.key)) {
      heap.push(std::move(head));
    }
  }

  return true;
}

} // namespace csvsort
