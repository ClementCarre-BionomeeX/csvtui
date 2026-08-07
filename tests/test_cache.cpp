#include "test_util.h"

#include "csv_cache.h"
#include "csv_model.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <vector>

namespace {

// Points the cache somewhere disposable for the duration of a test, so nothing
// here can read or write the user's real one.
class ScopedCacheDir {
public:
  ScopedCacheDir() {
    char pattern[] = "/tmp/csvtui-cache-test-XXXXXX";
    const char *made = ::mkdtemp(pattern);
    path_ = made != nullptr ? made : "";
    const char *previous = ::getenv("CSVTUI_CACHE_DIR");
    had_previous_ = previous != nullptr;
    if (had_previous_)
      previous_ = previous;
    ::setenv("CSVTUI_CACHE_DIR", path_.c_str(), 1);
  }

  ~ScopedCacheDir() {
    if (!path_.empty()) {
      const std::string command = "rm -rf " + path_;
      const int status = std::system(command.c_str());
      (void)status;
    }
    if (had_previous_)
      ::setenv("CSVTUI_CACHE_DIR", previous_.c_str(), 1);
    else
      ::unsetenv("CSVTUI_CACHE_DIR");
  }

  const std::string &path() const { return path_; }

private:
  std::string path_;
  std::string previous_;
  bool had_previous_ = false;
};

// The cache ignores files below a size threshold, so a test that wants one
// written has to produce something worth indexing.
std::string BigEnoughCsv(size_t rows) {
  std::string out = "id,name,filler\n";
  out.reserve(40ull * 1024 * 1024);
  const std::string padding(200, 'x');
  for (size_t i = 0; i < rows; ++i) {
    out += std::to_string(i);
    out += ",name";
    out += std::to_string(i);
    out += ',';
    out += padding;
    out += '\n';
  }
  return out;
}

csvcache::Key KeyFor(const std::string &path) {
  csvcache::Key key;
  CHECK(csvcache::DescribeFile(path, ',', true, CSVModel::kChunkSize, key));
  return key;
}

} // namespace

TEST(CacheRoundTripsAnIndex) {
  ScopedCacheDir cache;
  TempCSV file(BigEnoughCsv(200000)); // ~40 MB, past the size threshold

  const csvcache::Key key = KeyFor(file.path());
  csvcache::Index written;
  written.total_rows = 200000;
  for (int i = 0; i < 400; ++i)
    written.offsets.push_back(std::streampos(i * 1000));

  CHECK(csvcache::Save(key, written));

  csvcache::Index read;
  CHECK(csvcache::Load(key, read));
  CHECK_EQ(read.total_rows, written.total_rows);
  CHECK_EQ(read.offsets.size(), written.offsets.size());
  CHECK(read.offsets == written.offsets);
}

TEST(CacheMissesWhenTheFileChanges) {
  ScopedCacheDir cache;
  TempCSV file(BigEnoughCsv(200000));

  csvcache::Index index;
  index.total_rows = 200000;
  index.offsets.push_back(std::streampos(0));
  CHECK(csvcache::Save(KeyFor(file.path()), index));

  csvcache::Index read;
  CHECK(csvcache::Load(KeyFor(file.path()), read));

  // Touching the file changes its modification time, and the index no longer
  // describes it. A stale hit would put the viewer on the wrong rows silently,
  // which is far worse than counting again.
  struct utimbuf times {};
  times.actime = 1;
  times.modtime = 1;
  CHECK_EQ(::utime(file.path().c_str(), &times), 0);
  CHECK(!csvcache::Load(KeyFor(file.path()), read));
}

TEST(CacheMissesWhenTheReadingChanges) {
  ScopedCacheDir cache;
  TempCSV file(BigEnoughCsv(200000));

  csvcache::Index index;
  index.total_rows = 200000;
  index.offsets.push_back(std::streampos(0));
  CHECK(csvcache::Save(KeyFor(file.path()), index));

  // The same bytes read a different way are a different set of rows.
  csvcache::Key other;
  CHECK(csvcache::DescribeFile(file.path(), ';', true, CSVModel::kChunkSize,
                               other));
  csvcache::Index read;
  CHECK(!csvcache::Load(other, read));

  CHECK(csvcache::DescribeFile(file.path(), ',', false, CSVModel::kChunkSize,
                               other));
  CHECK(!csvcache::Load(other, read));

  CHECK(csvcache::DescribeFile(file.path(), ',', true, 64, other));
  CHECK(!csvcache::Load(other, read));
}

TEST(CacheRejectsDamagedFiles) {
  ScopedCacheDir cache;
  TempCSV file(BigEnoughCsv(200000));
  const csvcache::Key key = KeyFor(file.path());

  csvcache::Index index;
  index.total_rows = 200000;
  index.offsets.push_back(std::streampos(0));
  CHECK(csvcache::Save(key, index));

  // Truncate it half way through and it must be missed, not half-read.
  const std::string path = csvcache::PathFor(key);
  CHECK(!path.empty());
  CHECK_EQ(::truncate(path.c_str(), 20), 0);

  csvcache::Index read;
  CHECK(!csvcache::Load(key, read));

  // Garbage where the magic should be is likewise a miss.
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "not an index at all, but long enough to read past the header";
  }
  CHECK(!csvcache::Load(key, read));
}

// A damaged count must not become an allocation. The offset count is bounded
// by what the file's size could possibly hold, so a corrupt one is rejected
// rather than believed.
TEST(CacheRejectsAnImpossibleOffsetCount) {
  ScopedCacheDir cache;
  TempCSV file(BigEnoughCsv(200000));
  const csvcache::Key key = KeyFor(file.path());

  csvcache::Index index;
  index.total_rows = 200000;
  index.offsets.push_back(std::streampos(0));
  CHECK(csvcache::Save(key, index));

  const std::string path = csvcache::PathFor(key);
  std::vector<char> bytes;
  {
    std::ifstream in(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
  }
  CHECK(!bytes.empty());

  // The count is the last 8-byte field before the offsets themselves.
  const size_t count_at = bytes.size() - sizeof(std::int64_t) - sizeof(std::uint64_t);
  const std::uint64_t absurd = 1ull << 60;
  std::memcpy(bytes.data() + count_at, &absurd, sizeof(absurd));
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  csvcache::Index read;
  CHECK(!csvcache::Load(key, read));
}

TEST(CacheSkipsFilesTooSmallToBeWorthIt) {
  ScopedCacheDir cache;
  TempCSV file("id,name\n1,a\n2,b\n");

  csvcache::Index index;
  index.total_rows = 2;
  index.offsets.push_back(std::streampos(0));
  // Indexing a file that reads in a millisecond would only litter the cache.
  CHECK(!csvcache::Save(KeyFor(file.path()), index));
  csvcache::Index read;
  CHECK(!csvcache::Load(KeyFor(file.path()), read));
}

// --- what the model does with it ---------------------------------------------

TEST(ModelSavesAndReloadsItsIndex) {
  ScopedCacheDir cache;
  TempCSV file(BigEnoughCsv(200000));

  size_t counted = 0;
  {
    CSVModel model;
    CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
    CHECK(!model.RowCountIsExact()); // nothing cached yet
    counted = model.EnsureTotalRowCount();
    CHECK_EQ(counted, size_t{200000});
    CHECK(model.SaveIndex());
  }

  {
    CSVModel reopened;
    CHECK_EQ(reopened.Open(file.path(), {}, {}), std::string(""));
    // The whole point: exact before anything has been read.
    CHECK(reopened.RowCountIsExact());
    CHECK(reopened.row_count_came_from_cache());
    CHECK_EQ(reopened.RowCount(), counted);

    // And the offsets have to actually work, not merely be present.
    std::vector<std::string> fields;
    CHECK(reopened.GetRow(199999, fields));
    CHECK_EQ(fields[1], std::string("name199999"));
    CHECK(reopened.GetRow(100000, fields));
    CHECK_EQ(fields[1], std::string("name100000"));
    CHECK(!reopened.GetRow(200000, fields));
  }
}

TEST(ModelDoesNotRewriteAnIndexItJustLoaded) {
  ScopedCacheDir cache;
  TempCSV file(BigEnoughCsv(200000));

  {
    CSVModel model;
    CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
    model.EnsureTotalRowCount();
    CHECK(model.SaveIndex());
  }

  CSVModel reopened;
  CHECK_EQ(reopened.Open(file.path(), {}, {}), std::string(""));
  CHECK(reopened.row_count_came_from_cache());
  // Nothing new was learned, so there is nothing to write back.
  CHECK(!reopened.SaveIndex());
}

TEST(ModelIgnoresAnIndexForAModifiedFile) {
  ScopedCacheDir cache;
  TempCSV file(BigEnoughCsv(200000));

  {
    CSVModel model;
    CHECK_EQ(model.Open(file.path(), {}, {}), std::string(""));
    model.EnsureTotalRowCount();
    CHECK(model.SaveIndex());
  }

  // Append to the file: same name, different contents, different row count.
  {
    std::ofstream out(file.path(), std::ios::binary | std::ios::app);
    out << "extra,row,here\n";
  }

  CSVModel reopened;
  CHECK_EQ(reopened.Open(file.path(), {}, {}), std::string(""));
  CHECK(!reopened.row_count_came_from_cache());
  CHECK_EQ(reopened.EnsureTotalRowCount(), size_t{200001});
}
