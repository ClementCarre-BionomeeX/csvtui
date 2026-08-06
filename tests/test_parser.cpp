#include "test_util.h"

#include "csv_parser.h"

#include <sstream>

TEST(SplitPlainRecord) {
  const auto fields = csv::SplitRecord("a,b,c", ',');
  CHECK_EQ(fields.size(), size_t{3});
  CHECK_EQ(fields[0], std::string("a"));
  CHECK_EQ(fields[2], std::string("c"));
}

// Regression: a comma inside a quoted field used to create an extra column and
// shift every value after it.
TEST(QuotedFieldKeepsDelimiter) {
  const auto fields = csv::SplitRecord("1,\"Dupont, Jean\",Geneve,12", ',');
  CHECK_EQ(fields.size(), size_t{4});
  CHECK_EQ(fields[1], std::string("Dupont, Jean"));
  CHECK_EQ(fields[2], std::string("Geneve"));
  CHECK_EQ(fields[3], std::string("12"));
}

TEST(EscapedQuotesAreUnescaped) {
  const auto fields = csv::SplitRecord("1,\"say \"\"hi\"\"\",x", ',');
  CHECK_EQ(fields.size(), size_t{3});
  CHECK_EQ(fields[1], std::string("say \"hi\""));
}

TEST(EmptyFieldsArePreserved) {
  const auto fields = csv::SplitRecord(",,", ',');
  CHECK_EQ(fields.size(), size_t{3});
  CHECK_EQ(fields[0], std::string(""));
}

TEST(QuoteInsideUnquotedFieldIsLiteral) {
  const auto fields = csv::SplitRecord("a\"b,c", ',');
  CHECK_EQ(fields.size(), size_t{2});
  CHECK_EQ(fields[0], std::string("a\"b"));
}

TEST(DetectDelimiterIgnoresQuotedText) {
  // Six commas live inside the quotes; the real delimiter is the semicolon.
  const std::string line = "id;\"a,b,c,d,e,f\";z";
  CHECK_EQ(csv::DetectDelimiter(line), ';');
}

TEST(DetectDelimiterDefaultsToComma) {
  CHECK_EQ(csv::DetectDelimiter("single"), ',');
  CHECK_EQ(csv::DetectDelimiter("a,b,c"), ',');
  CHECK_EQ(csv::DetectDelimiter("a\tb\tc"), '\t');
}

TEST(ReadRecordStripsCarriageReturn) {
  std::istringstream in("a,b\r\nc,d\r\n");
  std::string record;
  CHECK(csv::ReadRecord(in, record));
  CHECK_EQ(record, std::string("a,b"));
  CHECK(csv::ReadRecord(in, record));
  CHECK_EQ(record, std::string("c,d"));
  CHECK(!csv::ReadRecord(in, record));
}

TEST(ReadRecordJoinsNewlineInsideQuotes) {
  std::istringstream in("a,\"line one\nline two\",b\nnext,row,here\n");
  std::string record;
  CHECK(csv::ReadRecord(in, record));
  CHECK_EQ(record, std::string("a,\"line one\nline two\",b"));

  const auto fields = csv::SplitRecord(record, ',');
  CHECK_EQ(fields.size(), size_t{3});
  CHECK_EQ(fields[1], std::string("line one\nline two"));

  CHECK(csv::ReadRecord(in, record));
  CHECK_EQ(record, std::string("next,row,here"));
}

TEST(ReadRecordAtEofWithUnterminatedQuote) {
  std::istringstream in("a,\"never closed\n");
  std::string record;
  CHECK(csv::ReadRecord(in, record)); // must not spin forever
  CHECK(!csv::ReadRecord(in, record));
}

TEST(StripBomRemovesMarker) {
  std::string with_bom = "\xEF\xBB\xBFid,name";
  csv::StripBom(with_bom);
  CHECK_EQ(with_bom, std::string("id,name"));

  std::string without = "id,name";
  csv::StripBom(without);
  CHECK_EQ(without, std::string("id,name"));
}

// Regression: widths were measured in bytes, so accented columns misaligned.
TEST(DisplayWidthCountsGlyphsNotBytes) {
  CHECK_EQ(csv::DisplayWidth("Geneve"), 6);
  CHECK_EQ(std::string("Genève").size(), size_t{7}); // 7 bytes...
  CHECK_EQ(csv::DisplayWidth("Genève"), 6);          // ...but 6 columns
  CHECK_EQ(csv::DisplayWidth("Zürich"), 6);
}

TEST(TruncateNeverSplitsAMultibyteCharacter) {
  const std::string value = "Genève";
  const std::string cut = csv::TruncateToWidth(value, 4);
  CHECK_EQ(csv::DisplayWidth(cut), 4);
  CHECK(cut.find("…") != std::string::npos);
  // A split UTF-8 sequence would leave a lone continuation byte behind.
  CHECK_EQ(csv::TruncateToWidth(value, 10), value);
  CHECK_EQ(csv::TruncateToWidth(value, 0), std::string(""));
}

TEST(SanitizeReplacesControlCharacters) {
  const std::string cleaned = csv::SanitizeForDisplay(std::string("a\x01""b"));
  CHECK(cleaned.find('\x01') == std::string::npos);
  CHECK_EQ(csv::SanitizeForDisplay("plain"), std::string("plain"));
}

TEST(NumberDetection) {
  CHECK(csv::IsNumeric("42"));
  CHECK(csv::IsNumeric("-3.5"));
  CHECK(csv::IsNumeric(" 7 "));
  CHECK(!csv::IsNumeric(""));
  CHECK(!csv::IsNumeric("12abc"));
  CHECK(!csv::IsNumeric("N/A"));
}

TEST(SmartCaseSearch) {
  CHECK(csv::SmartCaseInsensitive("alice"));
  CHECK(!csv::SmartCaseInsensitive("Alice"));

  CHECK_EQ(csv::FindFrom("Alice", "alice", 0, true), size_t{0});
  CHECK(csv::FindFrom("Alice", "alice", 0, false) == std::string::npos);
  CHECK_EQ(csv::FindFrom("abcabc", "abc", 1, false), size_t{3});
  CHECK_EQ(csv::FindLastBefore("abcabc", "abc", 5, false), size_t{3});
}

// Regression: backspace used to delete one byte, splitting accented input.
TEST(PrevCharBoundaryWalksWholeCharacters) {
  const std::string value = "café"; // 5 bytes, 4 characters
  CHECK_EQ(value.size(), size_t{5});
  const size_t back_one = csv::PrevCharBoundary(value, value.size());
  CHECK_EQ(back_one, size_t{3});
  CHECK_EQ(value.substr(0, back_one), std::string("caf"));
  CHECK_EQ(csv::PrevCharBoundary("abc", 3), size_t{2});
  CHECK_EQ(csv::PrevCharBoundary("", 0), size_t{0});
}
