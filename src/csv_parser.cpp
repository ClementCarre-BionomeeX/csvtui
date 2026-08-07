#include "csv_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <ftxui/screen/string.hpp>

namespace csv {
namespace {

// Every ctype call goes through this: passing a negative char is undefined
// behaviour and segfaults on libcs whose table is not padded below zero.
inline unsigned char AsByte(char c) { return static_cast<unsigned char>(c); }

inline char LowerAscii(char c) {
  unsigned char b = AsByte(c);
  return static_cast<char>(std::tolower(b));
}

inline bool IsContinuationByte(char c) { return (AsByte(c) & 0xC0) == 0x80; }

} // namespace

char DetectDelimiter(const std::string &line) {
  const char candidates[] = {',', ';', '\t', '|'};
  char best = ',';
  size_t best_count = 0;

  for (char candidate : candidates) {
    size_t count = 0;
    bool in_quotes = false;
    for (char c : line) {
      if (c == '"')
        in_quotes = !in_quotes;
      else if (c == candidate && !in_quotes)
        ++count;
    }
    if (count > best_count) {
      best_count = count;
      best = candidate;
    }
  }
  return best;
}

std::vector<std::string> SplitRecord(const std::string &record, char delimiter) {
  std::vector<std::string> fields;
  std::string buffer;
  detail::ForEachField(record, delimiter, buffer,
                       [&](size_t, const std::string &value) {
                         fields.push_back(value);
                         return true;
                       });
  return fields;
}

void ExtractField(const std::string &record, char delimiter, size_t index,
                  std::string &out, std::string &scratch) {
  bool found = false;
  detail::ForEachField(record, delimiter, scratch,
                       [&](size_t i, const std::string &value) {
                         if (i < index)
                           return true;
                         out.assign(value);
                         found = true;
                         return false;
                       });
  if (!found)
    out.clear();
}

void AppendQuoted(std::string &out, const std::string &value, char delimiter) {
  bool needs_quotes = false;
  for (char c : value) {
    if (c == delimiter || c == '"' || c == '\n' || c == '\r') {
      needs_quotes = true;
      break;
    }
  }

  if (!needs_quotes) {
    out += value;
    return;
  }

  out.push_back('"');
  for (char c : value) {
    if (c == '"')
      out.push_back('"'); // doubled, per RFC 4180
    out.push_back(c);
  }
  out.push_back('"');
}

bool RecordContains(const std::string &record, char delimiter,
                    const std::string &needle, bool ignore_case,
                    std::string &scratch) {
  bool hit = false;
  detail::ForEachField(record, delimiter, scratch,
                       [&](size_t, const std::string &value) {
                         if (FindFrom(value, needle, 0, ignore_case) ==
                             std::string::npos)
                           return true;
                         hit = true;
                         return false;
                       });
  return hit;
}

bool ReadRecord(std::istream &in, std::string &out) {
  out.clear();
  std::string line;
  bool in_quotes = false;
  bool read_any = false;

  while (std::getline(in, line)) {
    read_any = true;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (!out.empty())
      out.push_back('\n'); // newline that lived inside a quoted field
    out += line;

    for (char c : line) {
      if (c == '"')
        in_quotes = !in_quotes;
    }
    if (!in_quotes)
      return true;
  }

  // EOF with an unterminated quote: return what we have rather than looping.
  return read_any;
}

void StripBom(std::string &s) {
  if (s.size() >= 3 && AsByte(s[0]) == 0xEF && AsByte(s[1]) == 0xBB &&
      AsByte(s[2]) == 0xBF)
    s.erase(0, 3);
}

int DisplayWidth(const std::string &s) {
  return ftxui::string_width(s);
}

std::string TruncateToWidth(const std::string &s, int max_width) {
  if (max_width <= 0)
    return std::string();
  if (DisplayWidth(s) <= max_width)
    return s;
  if (max_width == 1)
    return "…";

  const int budget = max_width - 1; // room for the ellipsis
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    size_t next = i + 1;
    while (next < s.size() && IsContinuationByte(s[next]))
      ++next;
    const std::string glyph = s.substr(i, next - i);
    if (DisplayWidth(out + glyph) > budget)
      break;
    out += glyph;
    i = next;
  }
  out += "…";
  return out;
}

std::string SanitizeForDisplay(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    const unsigned char b = AsByte(c);
    if (b == '\t')
      out += "    ";
    else if (b < 0x20 || b == 0x7F)
      out += "·"; // keep the cell width honest instead of emitting raw controls
    else
      out.push_back(c);
  }
  return out;
}

bool ParseNumber(const std::string &s, double &out) {
  if (s.empty())
    return false;
  size_t begin = s.find_first_not_of(" \t");
  size_t end = s.find_last_not_of(" \t");
  if (begin == std::string::npos)
    return false;
  const std::string trimmed = s.substr(begin, end - begin + 1);
  if (trimmed.empty())
    return false;

  char *stop = nullptr;
  const double value = std::strtod(trimmed.c_str(), &stop);
  if (stop == nullptr || *stop != '\0')
    return false;
  out = value;
  return true;
}

bool IsNumeric(const std::string &s) {
  double ignored = 0.0;
  return ParseNumber(s, ignored);
}

bool SmartCaseInsensitive(const std::string &pattern) {
  for (char c : pattern) {
    const unsigned char b = AsByte(c);
    if (b < 0x80 && std::isupper(b))
      return false;
  }
  return true;
}

size_t FindFrom(const std::string &haystack, const std::string &needle,
                size_t from, bool ignore_case) {
  if (needle.empty() || from > haystack.size())
    return std::string::npos;
  if (!ignore_case)
    return haystack.find(needle, from);

  auto equal = [](char a, char b) { return LowerAscii(a) == LowerAscii(b); };
  auto it = std::search(haystack.begin() + static_cast<long>(from),
                        haystack.end(), needle.begin(), needle.end(), equal);
  if (it == haystack.end())
    return std::string::npos;
  return static_cast<size_t>(it - haystack.begin());
}

size_t FindLastBefore(const std::string &haystack, const std::string &needle,
                      size_t before, bool ignore_case) {
  if (needle.empty() || haystack.empty())
    return std::string::npos;
  const size_t limit = std::min(before, haystack.size());

  size_t best = std::string::npos;
  size_t at = FindFrom(haystack, needle, 0, ignore_case);
  while (at != std::string::npos && at <= limit) {
    best = at;
    if (at + 1 > haystack.size())
      break;
    at = FindFrom(haystack, needle, at + 1, ignore_case);
  }
  return best;
}

size_t PrevCharBoundary(const std::string &s, size_t index) {
  if (index == 0)
    return 0;
  size_t i = std::min(index, s.size());
  --i;
  while (i > 0 && IsContinuationByte(s[i]))
    --i;
  return i;
}

} // namespace csv
