# Changelog

## Unreleased

- **Write the current view to a file** with `w`: the rows the filter and sort
  have left, with the columns not hidden, quoted so it reads back unchanged and
  using the delimiter the source had. Runs on a worker with progress, refuses to
  overwrite an existing file, and removes the partial one if cancelled.
- **Search says how many matches there are.** The jump still happens at the
  first hit; the total needs a full pass, so it runs afterwards on its own file
  handle and arrives a moment later without blocking anything.
- **Columns can be resized.** `<` and `>` adjust the cursor column, `=` fits it
  to what is on screen, `X` restores the sampled widths. Widths are sampled from
  the first thousand rows so the table does not reflow while scrolling; these are
  for when that guess is wrong.
- **An active filter now outranks the cursor column in the status bar.** A
  filtered *and* sorted view used to drop the filter to make room, leaving the
  row count as the only hint that what was on screen was not the whole file.
- **The help overlay no longer hides keys.** Twenty-four bindings in a
  twenty-four row terminal silently lost the last three — which included `Esc`
  and `q`, so the overlay explaining the keys was concealing how to quit. It is
  now two columns, and a test asserts every binding appears.

## 0.3.0 — 2026-08-07

The release where csvtui became usable on the files it was built for. Every
figure below was measured on a 2.0 GB export of 23 769 659 rows and seven
columns, in a release build.

### Fixed

- **A crash on non-ASCII input.** Typing an accented character at a prompt
  passed a negative `char` to a `<cctype>` function, which is undefined
  behaviour: glibc absorbs it, and the libc on macOS and the BSDs segfaults.
  This is why the reports said "some characters" and "some people", and why it
  could not be reproduced on Linux. Every byte now goes through an explicit
  cast, and a pty fuzz harness runs on macOS in CI so the class cannot come
  back.
- **Unbounded memory growth while browsing.** The chunk cache had no ceiling.
  One failed search over a 46 MB file took the process from 4.3 MB to 224 MB;
  it is now capped at about 13 MB regardless.
- **Multi-byte characters split at prompts.** Only the first byte of a
  keystroke was appended, corrupting any pattern containing one.
- **Quitting.** `q` called `exit(0)`, bypassing the terminal restoration that
  `ScreenInteractive::Loop` does on the way out.

### Large files

- **Sorting no longer has a ceiling.** It held one key per row — about 61
  bytes, so roughly 9 GB for a 12 GB export — and was refused outright.
  It now fills a bounded buffer, sorts it, writes it out as a run, and merges
  the runs at the end, so the only thing that grows with the file is the answer
  itself at eight bytes a row. Peaks at 1029 MB on a roomy machine and 349 MB
  when told memory is tight, in the same 12 seconds either way. Temporary runs
  honour `CSVTUI_TMPDIR` and are removed even if you cancel.
- **Only the column you asked for is parsed.** Sorting by one column of seven
  used to build all seven fields of every row and discard six. Sort: 21.7 s →
  14.6 s and 1907 MB → 1455 MB. Filtered sort: 21.2 s → 10.8 s.
- **One streaming pass for everything.** Counting, filtering, sorting and
  column statistics are the same read with different things accumulated, so
  filter-then-sort is a single pass and the exact row count falls out of any of
  them. Sorting a file of unknown length used to count it first and then sort
  it — two full reads to answer one question.
- **The index outlives the session.** The chunk offset table is written to
  `~/.cache/csvtui` (364 kB for 2 GB, about 2.5 MB for 12 GB). Reopening a file
  starts with an exact row count and nothing read. Size, modification time,
  delimiter, header setting and resolved path are all checked; a mismatch is
  ignored rather than repaired.
- **Row counts are estimated until needed**, shown with a `~`. Searching and
  scrolling never trigger a count.
- **Filtering estimates its cost and refuses if it will not fit**, with the
  numbers, rather than exhausting memory. `CSVTUI_MEMORY_LIMIT` overrides what
  the system reports.

### Staying responsive

- **Nothing blocks the interface.** Counting, sorting, filtering and statistics
  run on a worker thread with progress and `Esc` to cancel; the grid stays
  scrollable throughout.
- **Search runs on a worker too.** A pattern that is absent takes as long as
  reading the file — 19 s on 2 GB — and used to freeze everything for all of
  it. Because a search produces a cursor position rather than a view, the grid
  is dimmed and no key but `Esc` is accepted while it runs. That restriction is
  also what makes it safe: nothing on the UI thread touches the model while the
  worker has it.
- **The readout ticks on a timer**, not on a row count, so it moves at the same
  rate whatever the file, with a spinner beside it.
- **A spilled sort names its merge phase.** It used to go silent at 100% for
  several seconds while merging — the worst possible moment to stop talking.

### Interface

- Cursor highlighting, a pinned header, and search-match highlighting that
  distinguishes the current hit from the others.
- Column statistics (`c`), which describe the filtered view rather than the
  whole file.
- Hide and show columns (`x` / `X`), freeze columns (`z`), full-cell detail
  (`Enter`), clipboard copy over OSC 52 (`y`), and a help overlay (`?`).
- A status bar that drops segments by importance instead of overflowing a
  narrow terminal, with messages on their own reserved line.
- Sorting places numbers before text in both directions, so empty and
  non-numeric cells collect at the end either way. Rows with equal keys keep
  their file order, so sorting by one column and then another refines rather
  than scrambles.

### Command line

- `--help`, `--version`, `-d/--delimiter` (accepting `tab` and `\t`),
  `--no-header`, `--header`, and `-` to read from a pipe.

### Testing

- 104 tests, up from 51, including twelve screen snapshots that pin the layout
  at 24, 40 and 80 columns, and two properties: nothing may exceed the terminal
  width, and a terminal too small to use must still render.
- A pty fuzz harness that drives the real binary, typing accented characters
  and control bytes at its prompts. Runs on macOS as well as Linux under the
  address and undefined-behaviour sanitizers.
- A libFuzzer target for the parser and the scanning pass, asserting that the
  single-column path agrees with full record splitting on arbitrary bytes.
- CI runs the address, undefined-behaviour and thread sanitizers.

### Environment

| Variable | Meaning |
| --- | --- |
| `CSVTUI_MEMORY_LIMIT` | Bytes csvtui may consider available. A lower limit means more spilling, not a refusal. |
| `CSVTUI_TMPDIR` | Where a large sort writes its temporary runs. |
| `CSVTUI_CACHE_DIR` | Where chunk offset tables are kept between sessions. |

## 0.2.0 and earlier

Initial versions: chunked reading, vim-style navigation, forward and reverse
search, sorting and filtering. Not separately tagged.
