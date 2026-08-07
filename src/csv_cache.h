#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// Remembering where the rows are, between one session and the next.
//
// The chunk offset table is one entry per 512 rows, so a complete index of a
// 12 GB export is about 2.5 MB — small enough to keep, and expensive enough to
// rebuild that keeping it is worth doing. Every full pass produces one as a
// by-product, so the second time a file is opened the row count can be exact
// before the first frame is drawn.
//
// The risk is describing the wrong file. Anything that would change what the
// offsets mean — the file's contents, the delimiter, whether the first row is
// a header — is recorded and checked, and a mismatch simply misses rather than
// being repaired: rebuilding an index costs seconds, and trusting a stale one
// would put the viewer on the wrong rows with no sign anything was amiss.
namespace csvcache {

struct Key {
  std::string path; // absolute, resolved
  long long size = 0;
  long long mtime = 0;
  char delimiter = ',';
  bool has_header = true;
  size_t chunk_size = 0;
};

struct Index {
  std::vector<std::streampos> offsets;
  size_t total_rows = 0;
};

// Resolves `path` and stats it. False when the file cannot be described, in
// which case nothing should be cached for it.
bool DescribeFile(const std::string &path, char delimiter, bool has_header,
                  size_t chunk_size, Key &out);

// Where indexes live: $CSVTUI_CACHE_DIR, else $XDG_CACHE_HOME/csvtui, else
// ~/.cache/csvtui. Empty when there is nowhere sensible to write, which is not
// an error — it just means indexes are not kept.
std::string Directory();

// The file an index for `key` would live in. Empty when Directory() is.
std::string PathFor(const Key &key);

// Reads a matching index. False when there is none, when it describes a
// different file, or when it is damaged.
bool Load(const Key &key, Index &out);

// Writes the index, creating the cache directory if needed. False on any I/O
// problem, which callers should ignore: failing to cache is not a failure.
bool Save(const Key &key, const Index &index);

} // namespace csvcache
