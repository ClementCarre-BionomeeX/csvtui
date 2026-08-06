#pragma once

#include <istream>
#include <string>
#include <vector>

// Pure CSV/text helpers, free of any UI dependency so they can be unit tested.
namespace csv {

// Picks the most likely delimiter from a header line, ignoring characters that
// appear inside quoted fields.
char DetectDelimiter(const std::string &line);

// Splits one logical record into fields following RFC 4180: quoted fields may
// contain the delimiter and newlines, and "" is an escaped quote.
std::vector<std::string> SplitRecord(const std::string &record, char delimiter);

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

} // namespace csv
