#include "test_util.h"

#include "csv_parser.h"
#include "csv_scan.h"

#include <sstream>
#include <string>
#include <vector>

// Writing a view back out.
//
// The property that matters is the round trip: whatever csvtui writes, csvtui
// must read back as the same values. Quoting is where that goes wrong, so most
// of what follows is about values that need quotes and values that must not
// get them.
namespace {

std::string Quoted(const std::vector<std::string> &fields, char delimiter) {
  std::string line;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i != 0)
      line.push_back(delimiter);
    csv::AppendQuoted(line, fields[i], delimiter);
  }
  return line;
}

} // namespace

TEST(QuotingOnlyHappensWhenItMust) {
  std::string out;
  csv::AppendQuoted(out, "plain", ',');
  CHECK_EQ(out, std::string("plain"));

  out.clear();
  csv::AppendQuoted(out, "", ',');
  CHECK_EQ(out, std::string(""));

  // A quote is not special unless it is there.
  out.clear();
  csv::AppendQuoted(out, "it's fine", ',');
  CHECK_EQ(out, std::string("it's fine"));

  // Spaces do not need quoting, and adding them would change the value for
  // anything that trims.
  out.clear();
  csv::AppendQuoted(out, " padded ", ',');
  CHECK_EQ(out, std::string(" padded "));
}

TEST(QuotingCoversEverythingThatWouldBreakAReader) {
  std::string out;
  csv::AppendQuoted(out, "Dupont, Jean", ',');
  CHECK_EQ(out, std::string("\"Dupont, Jean\""));

  out.clear();
  csv::AppendQuoted(out, "he said \"hi\"", ',');
  CHECK_EQ(out, std::string("\"he said \"\"hi\"\"\""));

  out.clear();
  csv::AppendQuoted(out, "Marie\nCurie", ',');
  CHECK_EQ(out, std::string("\"Marie\nCurie\""));

  out.clear();
  csv::AppendQuoted(out, "trailing\r", ',');
  CHECK_EQ(out, std::string("\"trailing\r\""));

  // The delimiter is whatever the file uses, so a comma is ordinary in a
  // semicolon-separated file and a semicolon is not.
  out.clear();
  csv::AppendQuoted(out, "a,b", ';');
  CHECK_EQ(out, std::string("a,b"));
  out.clear();
  csv::AppendQuoted(out, "a;b", ';');
  CHECK_EQ(out, std::string("\"a;b\""));
}

// The round trip, on exactly the values that have caused trouble.
TEST(WrittenFieldsReadBackUnchanged) {
  const std::vector<std::string> awkward = {
      "plain",         "",
      "Dupont, Jean",  "he said \"hi\"",
      "Marie\nCurie",  "Genève",
      "中文字",         " leading",
      "trailing ",     "\"already quoted\"",
      "a,b,c",         "semi;colon",
      "tab\there",     "1000",
  };

  for (char delimiter : {',', ';', '\t'}) {
    const std::string line = Quoted(awkward, delimiter);
    const std::vector<std::string> back = csv::SplitRecord(line, delimiter);
    CHECK_EQ(back.size(), awkward.size());
    for (size_t i = 0; i < awkward.size() && i < back.size(); ++i)
      CHECK_EQ(back[i], awkward[i]);
  }
}

TEST(AWrittenRecordSurvivesBeingReadFromAStream) {
  // Not just SplitRecord: a value containing a newline has to survive
  // ReadRecord's line joining too, which is the whole reason it gets quotes.
  const std::vector<std::string> fields = {"1", "Marie\nCurie", "Lyon"};
  std::string file = Quoted(fields, ',');
  file.push_back('\n');

  std::istringstream in(file);
  std::string record;
  CHECK(csv::ReadRecord(in, record));
  const std::vector<std::string> back = csv::SplitRecord(record, ',');
  std::string leftover;
  CHECK(!csv::ReadRecord(in, leftover)); // the newline did not split the row
  CHECK_EQ(back.size(), size_t{3});
  CHECK_EQ(back[1], std::string("Marie\nCurie"));
}

// --- counting matches --------------------------------------------------------

TEST(ScanCountsMatchesWithoutBuildingAnOrdering) {
  std::ostringstream csv;
  csv << "id,name\n";
  for (size_t i = 0; i < 300; ++i)
    csv << i << ',' << (i % 3 == 0 ? "alpha" : "bravo") << '\n';
  const std::string text = csv.str();
  TempCSV file(text);

  csvscan::Request request;
  request.path = file.path();
  request.data_offset = std::streampos(std::string("id,name\n").size());
  request.count_pattern = "alpha";

  csvscan::Result result;
  CHECK(csvscan::Run(request, result, nullptr, nullptr) ==
        csvscan::Outcome::Done);
  CHECK_EQ(result.matches, size_t{100});
  CHECK_EQ(result.total_rows, size_t{300});
  // Counting is not ordering: nothing was built that has to be kept.
  CHECK(!result.has_order);
  CHECK(result.order.empty());
}

TEST(MatchCountingRespectsAnActiveFilter) {
  std::ostringstream csv;
  csv << "id,name,city\n";
  for (size_t i = 0; i < 300; ++i) {
    csv << i << ',' << (i % 3 == 0 ? "alpha" : "bravo") << ','
        << (i % 2 == 0 ? "Lyon" : "Paris") << '\n';
  }
  const std::string text = csv.str();
  TempCSV file(text);

  csvscan::Request request;
  request.path = file.path();
  request.data_offset = std::streampos(std::string("id,name,city\n").size());
  request.count_pattern = "alpha";
  request.filter = true;
  request.filter_pattern = "Lyon";

  csvscan::Result result;
  CHECK(csvscan::Run(request, result, nullptr, nullptr) ==
        csvscan::Outcome::Done);
  // Rows that are both alpha (every third) and Lyon (every second): every
  // sixth, so fifty of three hundred.
  CHECK_EQ(result.matches, size_t{50});
}

TEST(MatchCountingIsSmartCaseLikeSearch) {
  TempCSV file("id,name\n1,Alpha\n2,alpha\n3,ALPHA\n");
  csvscan::Request request;
  request.path = file.path();
  request.data_offset = std::streampos(std::string("id,name\n").size());

  // All lowercase: matches regardless of case, as searching does.
  request.count_pattern = "alpha";
  csvscan::Result insensitive;
  CHECK(csvscan::Run(request, insensitive, nullptr, nullptr) ==
        csvscan::Outcome::Done);
  CHECK_EQ(insensitive.matches, size_t{3});

  // A capital makes it exact.
  request.count_pattern = "Alpha";
  csvscan::Result sensitive;
  CHECK(csvscan::Run(request, sensitive, nullptr, nullptr) ==
        csvscan::Outcome::Done);
  CHECK_EQ(sensitive.matches, size_t{1});
}
