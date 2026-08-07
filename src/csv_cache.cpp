#include "csv_cache.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace csvcache {
namespace {

// Bumped whenever the layout below changes, so an old cache is missed rather
// than misread.
constexpr char kMagic[8] = {'C', 'S', 'V', 'T', 'U', 'I', 'I', 'X'};
constexpr std::uint32_t kVersion = 1;

// A file has to be worth indexing. Below this, a rebuild is imperceptible and
// caching would only litter the cache directory.
constexpr long long kMinimumFileSize = 32ll * 1024 * 1024;

std::string Environment(const char *name) {
  const char *value = std::getenv(name);
  return (value != nullptr && value[0] != '\0') ? std::string(value)
                                                : std::string();
}

// FNV-1a. Only has to spread paths across filenames; the path itself is stored
// in the file and compared, so a collision misses rather than misleads.
std::uint64_t Hash(const std::string &text) {
  std::uint64_t hash = 1469598103934665603ull;
  for (char c : text) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string HexOf(std::uint64_t value) {
  static const char *digits = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<size_t>(i)] = digits[value & 0xF];
    value >>= 4;
  }
  return out;
}

bool MakeDirectories(const std::string &path) {
  if (path.empty())
    return false;
  std::string partial;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (!partial.empty() && ::mkdir(partial.c_str(), 0700) != 0 &&
          errno != EEXIST)
        return false;
    }
    if (i < path.size())
      partial.push_back(path[i]);
  }
  return true;
}

template <typename T> void Write(std::ostream &out, const T &value) {
  out.write(reinterpret_cast<const char *>(&value), sizeof(value));
}

template <typename T> bool Read(std::istream &in, T &value) {
  return static_cast<bool>(
      in.read(reinterpret_cast<char *>(&value), sizeof(value)));
}

} // namespace

bool DescribeFile(const std::string &path, char delimiter, bool has_header,
                  size_t chunk_size, Key &out) {
  char resolved[PATH_MAX];
  if (::realpath(path.c_str(), resolved) == nullptr)
    return false;

  struct stat info {};
  if (::stat(resolved, &info) != 0 || !S_ISREG(info.st_mode))
    return false;

  out.path = resolved;
  out.size = static_cast<long long>(info.st_size);
  out.mtime = static_cast<long long>(info.st_mtime);
  out.delimiter = delimiter;
  out.has_header = has_header;
  out.chunk_size = chunk_size;
  return true;
}

std::string Directory() {
  const std::string override_dir = Environment("CSVTUI_CACHE_DIR");
  if (!override_dir.empty())
    return override_dir;

  const std::string xdg = Environment("XDG_CACHE_HOME");
  if (!xdg.empty())
    return xdg + "/csvtui";

  const std::string home = Environment("HOME");
  if (!home.empty())
    return home + "/.cache/csvtui";

  return std::string();
}

std::string PathFor(const Key &key) {
  const std::string directory = Directory();
  if (directory.empty())
    return std::string();
  return directory + "/" + HexOf(Hash(key.path)) + ".idx";
}

bool Load(const Key &key, Index &out) {
  const std::string path = PathFor(key);
  if (path.empty())
    return false;

  std::ifstream in(path, std::ios::binary);
  if (!in.is_open())
    return false;

  char magic[sizeof(kMagic)] = {0};
  if (!in.read(magic, sizeof(magic)) ||
      std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
    return false;

  std::uint32_t version = 0;
  std::int64_t size = 0;
  std::int64_t mtime = 0;
  std::uint8_t delimiter = 0;
  std::uint8_t has_header = 0;
  std::uint64_t chunk_size = 0;
  std::uint64_t total_rows = 0;
  std::uint64_t path_length = 0;
  std::uint64_t count = 0;

  if (!Read(in, version) || version != kVersion)
    return false;
  if (!Read(in, size) || !Read(in, mtime) || !Read(in, delimiter) ||
      !Read(in, has_header) || !Read(in, chunk_size) || !Read(in, total_rows) ||
      !Read(in, path_length))
    return false;

  // Everything that would change what the offsets mean.
  if (size != key.size || mtime != key.mtime ||
      delimiter != static_cast<std::uint8_t>(key.delimiter) ||
      (has_header != 0) != key.has_header || chunk_size != key.chunk_size)
    return false;

  if (path_length > 4096)
    return false;
  std::string stored_path(static_cast<size_t>(path_length), '\0');
  if (path_length != 0 &&
      !in.read(&stored_path[0], static_cast<std::streamsize>(path_length)))
    return false;
  if (stored_path != key.path)
    return false; // a hash collision, or the cache directory is shared

  if (!Read(in, count))
    return false;
  // A record cannot be shorter than one byte, so the file's size divided by
  // the rows per chunk bounds how many offsets could possibly be real. Without
  // this a corrupt count would ask for an allocation of any size it liked.
  const std::uint64_t ceiling =
      chunk_size == 0 ? 0
                      : static_cast<std::uint64_t>(size) / chunk_size + 2;
  if (count > ceiling)
    return false;

  Index loaded;
  loaded.total_rows = static_cast<size_t>(total_rows);
  // Reserve what a plausible file needs and let the rest grow: the ceiling
  // above is generous enough that trusting it outright would still allow a
  // damaged cache to claim a few hundred megabytes.
  loaded.offsets.reserve(
      static_cast<size_t>(std::min<std::uint64_t>(count, 1u << 20)));
  for (std::uint64_t i = 0; i < count; ++i) {
    std::int64_t offset = 0;
    if (!Read(in, offset) || offset < 0 || offset > size)
      return false;
    loaded.offsets.push_back(std::streampos(offset));
  }
  if (loaded.offsets.empty())
    return false;

  out = std::move(loaded);
  return true;
}

bool Save(const Key &key, const Index &index) {
  if (index.offsets.empty() || key.size < kMinimumFileSize)
    return false;

  const std::string directory = Directory();
  const std::string path = PathFor(key);
  if (path.empty() || !MakeDirectories(directory))
    return false;

  // Write beside the target and rename, so a cache file is never half written
  // — a reader that found one would reject it, but only after being handed
  // something that looked plausible.
  const std::string temporary = path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
      return false;

    out.write(kMagic, sizeof(kMagic));
    Write(out, kVersion);
    Write(out, static_cast<std::int64_t>(key.size));
    Write(out, static_cast<std::int64_t>(key.mtime));
    Write(out, static_cast<std::uint8_t>(key.delimiter));
    Write(out, static_cast<std::uint8_t>(key.has_header ? 1 : 0));
    Write(out, static_cast<std::uint64_t>(key.chunk_size));
    Write(out, static_cast<std::uint64_t>(index.total_rows));
    Write(out, static_cast<std::uint64_t>(key.path.size()));
    out.write(key.path.data(), static_cast<std::streamsize>(key.path.size()));
    Write(out, static_cast<std::uint64_t>(index.offsets.size()));
    for (const std::streampos &offset : index.offsets)
      Write(out, static_cast<std::int64_t>(offset));

    out.flush();
    if (!out) {
      ::unlink(temporary.c_str());
      return false;
    }
  }

  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return false;
  }
  return true;
}

} // namespace csvcache
