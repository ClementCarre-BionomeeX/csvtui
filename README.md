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

## Build

Requires CMake ≥ 3.16 and a C++17 compiler. FTXUI is fetched automatically.

```sh
cmake -S . -B build
cmake --build build -j
./build/csvtui data.csv
```

Install system-wide with `cmake --install build` (defaults to `/usr/local`).

Run the tests with:

```sh
ctest --test-dir build --output-on-failure
```

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

Some things genuinely need to read everything, and csvtui is explicit about them
rather than freezing:

- **Row counts.** The status bar shows `~6 282 862` — a `~` estimate derived
  from the file size — until something needs the exact number. `G`, `<n>G` past
  the end, sorting, filtering and column statistics start a **background count**
  that reports progress and can be cancelled with `Esc`. The UI stays live
  throughout. Once counted, the offsets are kept, so every later jump is instant.
- **Sorting and filtering** hold one index entry per row: roughly 64 bytes per
  row to sort, 10 to filter. Before starting, csvtui estimates the cost and
  **refuses if it would not fit in memory**, telling you what it would need:

  ```
  sorting ~156 000 000 rows needs ~9.3 GB, only 4.2 GB usable — filter first
  ```

  Set `CSVTUI_MEMORY_LIMIT` (in bytes) to cap what csvtui considers available,
  which is useful on shared machines.

Searching and scrolling never trigger a count.

## Notes

Sorting and filtering still make one full pass over the file once started, so on
a multi-gigabyte file they take a while. They no longer risk exhausting memory.

## License

MIT — see [LICENSE](LICENSE).
