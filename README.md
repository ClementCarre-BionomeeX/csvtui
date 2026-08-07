# csvtui

A terminal viewer for CSV files, with vim-style navigation.

Point it at a CSV and it lays the file out as an aligned grid you can move
around with `hjkl`, search, sort and filter — without loading the whole file
into memory.

```
 id │name   │email               │role  │signup_date│active│ score
────────────────────────────────────────────────────────────────────
  1│User1  │user1@example.com   │Admin │2024-01-06 │true  │    57
  2│User2  │user2@example.com   │Editor│2024-01-11 │true  │    64
  3│User3  │user3@example.com   │Viewer│2024-01-16 │true  │    71
  4│User4  │user4@example.com   │Admin │2024-01-21 │false │    78
 test.csv │ row 3/200 (1%) │ col 4/7 role │ delim ','    ? help  q quit
```

## Why

- **Reads the file lazily.** Only a window of rows is ever resident, so opening
  a multi-gigabyte export is as fast as opening a small one.
- **Parses CSV properly.** Quoted fields containing commas or newlines, escaped
  `""` quotes, CRLF endings and byte order marks all behave.
- **Gets the alignment right.** Column widths are measured in terminal cells,
  not bytes, so accented and CJK text lines up.

## Install

Download a binary for your platform from the
[latest release](https://github.com/ClementCarre-BionomeeX/csvtui/releases/latest),
verify it against the published `SHA256SUMS`, and put it on your `PATH`:

```sh
tar xzf csvtui-linux-x86_64.tar.gz
sudo install -m755 csvtui-linux-x86_64/csvtui /usr/local/bin/
sudo install -m644 csvtui-linux-x86_64/csvtui.1 /usr/local/share/man/man1/
```

Two builds are published: `linux-x86_64`, and `macos-universal`, which is a
single binary carrying both Apple silicon and Intel slices.

## Build from source

Requires CMake ≥ 3.16 and a C++17 compiler. FTXUI is fetched automatically.

```sh
cmake -S . -B build
cmake --build build -j
./build/csvtui data.csv
```

Install system-wide with `cmake --install build` (defaults to `/usr/local`).
See [Tests](#tests) for the test suite, and [CHANGELOG.md](CHANGELOG.md) for
what changed.

## Usage

```sh
csvtui data.csv                 # open a file
csvtui -d ';' export.csv        # force a delimiter
csvtui -d tab data.tsv          # tab-separated
csvtui --no-header raw.csv      # first row is data, not a header
psql -c 'copy ... to stdout csv' | csvtui -    # read from a pipe
```

| Option | Meaning |
| --- | --- |
| `-d`, `--delimiter <char>` | Field delimiter. Accepts `tab` / `\t`. Auto-detected by default. |
| `--no-header` | Treat the first row as data. |
| `--header` | Force a header row (the default). |
| `-h`, `--help` | Show usage. |
| `-V`, `--version` | Show the version. |

## Keys

Press `?` in the viewer for this list.

| Key | Action |
| --- | --- |
| `h` `j` `k` `l`, arrows | Move the cursor |
| `Ctrl-D` / `Ctrl-U` | Half page down / up |
| `Ctrl-F` / `Ctrl-B`, `PgDn` / `PgUp` | Full page down / up |
| `gg` / `G` | First / last row |
| `<n>G` | Go to row *n* |
| `0` / `$` | First / last column |
| `/` | Search forward, then `Enter` |
| `n` / `N` | Next / previous match |
| `f` | Filter rows to those matching a pattern (empty clears) |
| `s` / `S` | Sort by the cursor column, ascending / descending |
| `u` | Clear sort and filter |
| `x` / `X` | Hide the cursor column / show all columns |
| `z` | Freeze columns up to the cursor (they stay put when scrolling right) |
| `Enter` | Show the full cell value |
| `y` | Copy the cell to the clipboard (OSC 52, works over SSH) |
| `c` | Column statistics: count, empties, min, max, mean |
| `H` | Pin or unpin the header |
| `t` | Toggle aligned and raw modes |
| `?` | Toggle help |
| `q` | Quit (`Esc` closes an overlay first) |

The mouse works too: the wheel scrolls and a click moves the cursor.

Search is *smart case*: an all-lowercase pattern matches case-insensitively, a
pattern with any capital matches exactly. Searches wrap around the end of the
file and say so in the status bar.

## Very large files

csvtui is built for files that other tools refuse to open. Browsing is O(1) in
the file size: opening a 12 GB export reads about a thousand rows and stops, and
scrolling stays at a few megabytes of RSS no matter how far you go.

Some things genuinely need to read everything, and csvtui is explicit about
them rather than freezing.

**Nothing blocks the interface.** Counting rows, sorting, filtering and column
statistics all run on a worker thread that reports progress and stops on `Esc`,
with the grid still scrollable. Each is a *single* pass: sorting a file whose
length is not yet known no longer counts it first and then sorts it.

Searching runs on a worker too, but differently. It produces a cursor position
rather than a view, so moving around while it looks for one makes no sense —
the grid is dimmed, no key but `Esc` is accepted, and the status line counts
the rows examined. A pattern that is not in the file takes as long as reading
the file, about 19 seconds for 2 GB, and you can stop it at any point.

The readout ticks on a timer rather than on a row count, so it moves at the
same rate whatever the file, and a spinner turns beside it: a number that has
not changed for a second looks identical to a program that has stopped.

**Row counts are estimated until they are needed.** The status bar shows
`~6 282 862` — the `~` means it was derived from the file size — until
something wants the exact number. Searching and scrolling never do.

**Only the column you asked about is parsed.** Sorting a seven-column file by
one column used to build all seven fields of every row and discard six. On a
2 GB file that alone took a sort from 21.7 s to 14.6 s, and a filtered sort
from 21.2 s to 10.8 s.

**Sorting spills to disk rather than refusing.** A sort holds a key per row
while it works, which on a 12 GB export is about 9 GB. Instead it fills a
bounded buffer, sorts it, writes it out as a run, and merges the runs at the
end — so the only thing that grows with the file is the answer itself, at eight
bytes a row. Sorting 23.7 million rows peaks at 1029 MB on a roomy machine and
349 MB when told memory is tight, in the same 12 seconds. Temporary runs go to
`$CSVTUI_TMPDIR`, else `$TMPDIR`, else `/tmp`, and are deleted even if you
cancel.

**Filtering** holds one index entry per row, roughly 10 bytes. csvtui estimates
that up front and refuses if it would not fit, saying what it would have
needed:

```
sorting ~156 000 000 rows needs ~9.3 GB, only 4.2 GB usable — filter first
```

Set `CSVTUI_MEMORY_LIMIT` (in bytes) to cap what csvtui considers available,
which is useful on shared machines. It also shrinks the sort buffer, so a lower
limit means more spilling rather than a refusal.

**The index outlives the session.** Once a file has been read through, its
offset table goes to `~/.cache/csvtui` — about 364 kB for a 2 GB file, 2.5 MB
for a 12 GB one. Open that file again and the row count is exact before the
first frame, with nothing read. The cache records the file's size, modification
time, delimiter and header setting; change any of them and it is ignored rather
than trusted. Point `CSVTUI_CACHE_DIR` elsewhere, or delete the directory, at
any time.

## Notes

Sorting and filtering read the whole file once started, so on a multi-gigabyte
file they take a while — they simply do it without blocking you, and `Esc`
abandons them.

Sorting puts numbers before text in both directions, so empty and non-numeric
cells collect at the end whether you sort with `s` or `S`. Rows whose sort key
is equal keep their order in the file, which means sorting by one column and
then another refines the result instead of scrambling it.

## Tests

```sh
ctest --test-dir build --output-on-failure   # unit tests
tests/fuzz/fuzz_tui.py ./build/csvtui        # drive the real UI through a pty
```

Some of those tests are screen snapshots: the view is rendered off-screen and
compared against a file in `tests/golden`. When a layout change is intended,
regenerate them and read the diff before committing:

```sh
CSVTUI_UPDATE_GOLDEN=1 ./build/csvtui_tests
git diff tests/golden
```

The pty harness types accented characters, control bytes and overlong strings
into the viewer's prompts and checks it still exits cleanly. It exists because
the one crash that reached users was undefined behaviour on a negative `char`,
which glibc absorbs and the libc on macOS turns into a segfault: no unit test
could see it, and no Linux machine could reproduce it. CI runs this harness on
macOS as well as Linux, under the address and undefined-behaviour sanitizers.

There is also a libFuzzer target for the parser and the scanning pass, off by
default because it needs Clang:

```sh
cmake -S . -B build-fuzz -DCSVTUI_BUILD_FUZZERS=ON -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined,fuzzer-no-link -g"
cmake --build build-fuzz -j --target fuzz_parser
./build-fuzz/fuzz_parser tests/fuzz/corpus -max_total_time=60
```

## License

MIT — see [LICENSE](LICENSE).
