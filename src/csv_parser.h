#pragma once

#include <istream>
#include <string>
#include <vector>

// Pure CSV/text helpers, free of any UI dependency so they can be unit tested.
namespace csv {

// Picks the most likely delimiter from a header line, ignoring characters that
// appear inside quoted fields.
char DetectDelimiter(const std::string &line);

namespace detail {

// The RFC 4180 field state machine, shared by everything below so there is only
// one definition of what a field is. `visit(index, value)` is called once per
// field and returns false to stop early; `buffer` holds the field being built
// and is left holding the last one visited, so a caller scanning a whole file
// can hand in the same string every time and stop allocating after a few rows.
template <typename Visitor>
void ForEachField(const std::string &record, char delimiter, std::string &buffer,
                  Visitor &&visit) {
  buffer.clear();
  size_t index = 0;
  bool in_quotes = false;
  bool field_started = false;

  for (size_t i = 0; i < record.size(); ++i) {
    const char c = record[i];
    if (in_quotes) {
      if (c != '"') {
        buffer.push_back(c);
        continue;
      }
      // "" inside a quoted field is a literal quote.
      if (i + 1 < record.size() && record[i + 1] == '"') {
        buffer.push_back('"');
        ++i;
      } else {
        in_quotes = false;
      }
      continue;
    }

    if (c == '"' && !field_started) {
      in_quotes = true;
      field_started = true;
    } else if (c == delimiter) {
      if (!visit(index, static_cast<const std::string &>(buffer)))
        return;
      ++index;
      buffer.clear();
      field_started = false;
    } else {
      buffer.push_back(c);
      field_started = true;
    }
  }

  visit(index, static_cast<const std::string &>(buffer));
}

} // namespace detail

// Splits one logical record into fields following RFC 4180: quoted fields may
// contain the delimiter and newlines, and "" is an escaped quote.
std::vector<std::string> SplitRecord(const std::string &record, char delimiter);

// Copies field `index` of the record into `out`, stopping as soon as it has it
// and never building the fields around it. Sorting a seven-column file by one
// column throws away six of every seven fields SplitRecord would have built;
// over millions of rows that is most of the work. `out` is empty when the
// record has no such field.
//
// `scratch` is the walker's working buffer, reused across calls. It is a
// separate string from `out` on purpose: it grows to the longest field walked
// *past* — an email column, say — and keeps that capacity. Handing it back as
// the result would give every one of a million sort keys a heap block sized
// for a field it does not hold.
void ExtractField(const std::string &record, char delimiter, size_t index,
                  std::string &out, std::string &scratch);

// True when any field of the record contains `needle`. Equivalent to searching
// each element of SplitRecord, but it stops at the first hit and unquotes into
// `scratch` rather than into a fresh vector of strings.
bool RecordContains(const std::string &record, char delimiter,
                    const std::string &needle, bool ignore_case,
                    std::string &scratch);

// Reads one logical record, consuming newlines that occur inside quoted
// fields. Trailing \r is stripped so CRLF files behave. Returns false at EOF.
bool ReadRecord(std::istream &in, std::string &out);

// Removes a leading UTF-8 byte order mark, if present.
void StripBom(std::string &s);

// Display width in terminal cells (accounts for UTF-8 and double-width glyphs).
int DisplayWidth(const std::string &s);

// Truncates to `max_width` cells, appending an ellipsis when it had to cut.
// Never splits a UTF-8 sequence.
std::string TruncateToWidth(const std::string &s, int max_width);

// Replaces C0 control characters, which would corrupt the rendered grid.
std::string SanitizeForDisplay(const std::string &s);

// True when the value parses as a number (used for right alignment / stats).
bool IsNumeric(const std::string &s);
bool ParseNumber(const std::string &s, double &out);

// Smart case: a pattern without uppercase matches case-insensitively.
bool SmartCaseInsensitive(const std::string &pattern);

// Substring search honouring the case-insensitive flag. npos when absent.
size_t FindFrom(const std::string &haystack, const std::string &needle,
                size_t from, bool ignore_case);
// Last occurrence starting at or before `before`.
size_t FindLastBefore(const std::string &haystack, const std::string &needle,
                      size_t before, bool ignore_case);

// Byte index of the start of the UTF-8 character preceding `index`.
size_t PrevCharBoundary(const std::string &s, size_t index);

// Appends `value` to `out` as a CSV field, quoting it only when it has to be:
// when it holds the delimiter, a quote, a newline or a carriage return. A
// value written this way reads back through SplitRecord unchanged, which is
// the only property that matters here.
void AppendQuoted(std::string &out, const std::string &value, char delimiter);

} // namespace csv
