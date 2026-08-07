#!/usr/bin/env python3
"""Drive the real csvtui binary through a pty with awkward input.

This is the harness that would have caught the crash that shipped. The bug was
that a UTF-8 lead byte is negative as a plain `char`, and passing one to a
<cctype> function is undefined behaviour: glibc happens to survive it, and the
libc on macOS and the BSDs does not. No unit test saw it, because the faulty
call was reached only by typing an accented character at a prompt inside a
running terminal UI.

So this drives the actual binary, in an actual pty, and types the things a
person types — including "é", which is exactly the key an AZERTY keyboard puts
under your finger.

    tests/fuzz/fuzz_tui.py ./build/csvtui                 # a quick pass
    tests/fuzz/fuzz_tui.py ./build/csvtui --rounds 200    # a longer one
    tests/fuzz/fuzz_tui.py ./build/csvtui --seed 12345    # reproduce a failure

Exit status is 0 when every round ended with the viewer quitting cleanly, and
1 as soon as one does not. The failing seed and the keystrokes that led there
are printed, so a failure is reproducible.
"""

import argparse
import os
import pty
import random
import select
import signal
import subprocess
import sys
import tempfile
import time

# Bytes that have caused trouble, or plausibly could. The accented characters
# are the ones behind the original crash report; the rest cover the C0 range,
# a stray UTF-8 continuation byte with no lead, an over-long sequence, and a
# lone surrogate-shaped encoding.
NASTY_INPUT = [
    "é", "à", "ü", "ß", "Ω", "中", "🙂", "​", "́",
    "\x01", "\x07", "\x1b", "\x7f", "\x00",
    "\udcff", "�",
    "'", '"', "\\", "%s", "%n", "../", "-", "..",
    "a" * 300,
]

# Keys the viewer actually binds, weighted towards navigation because that is
# what a session mostly is.
NAVIGATION = list("hjkl") * 4 + ["gg", "G", "0", "$", "\x04", "\x15", "\x06", "\x02"]
COMMANDS = list("nNsSuxXzHtc?y") + ["\r", "\x1b"]
PROMPTS = ["/", "f"]

HEADER = "id,name,email,city,role,score,note\n"

# How many times to ask the viewer to quit before calling it wedged.
QUIT_ATTEMPTS = 4


def build_csv(path, rows, rng):
    """A file with the shapes that have broken parsers: quoted delimiters,
    escaped quotes, embedded newlines, CRLF, a BOM, wide glyphs, empty cells."""
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write("﻿")  # byte order mark
        handle.write(HEADER)
        for i in range(rows):
            name = rng.choice([
                "User%d" % i,
                '"Dupont, Jean"',
                '"he said ""hi"""',
                '"Marie\nCurie"',
                "Genève",
                "中文字",
                "",
            ])
            note = rng.choice(["", "free text %d" % i, '"a, b, c"', "x" * 200])
            line = "%d,%s,user%d@example.com,Genève,%s,%d,%s" % (
                i, name, i, rng.choice(["Admin", "Editor", ""]), i % 100, note)
            handle.write(line + rng.choice(["\n", "\r\n"]))


def drain(fd, deadline):
    """Read whatever the child has produced, without blocking past `deadline`."""
    out = b""
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            break
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        out += chunk
    return out


def run_round(binary, csv_path, rng, keystrokes, timeout):
    """Returns (ok, detail). Sends `keystrokes`, then quits, then waits."""
    master, slave = pty.openpty()
    env = dict(os.environ, TERM="xterm-256color", LINES="24", COLUMNS="80")
    child = subprocess.Popen(
        [binary, csv_path], stdin=slave, stdout=slave, stderr=slave,
        env=env, close_fds=True)
    os.close(slave)

    try:
        drain(master, time.time() + 1.0)  # let it draw the first frame
        for keys in keystrokes:
            if child.poll() is not None:
                break
            try:
                os.write(master, keys.encode("utf-8", "surrogateescape"))
            except OSError:
                break
            drain(master, time.time() + 0.02)

        # Quitting takes more care than it looks.
        #
        # Escape is no use for this. A lone Escape leaves the terminal input
        # parser waiting to see whether a sequence follows it, and the bytes
        # that do follow are folded into an Alt-<key> nothing binds — so the
        # Escape never reaches the viewer at all, and a prompt left open stays
        # open. Enter does reach it, and always leaves a prompt; `q` then quits
        # from normal mode and from an overlay alike. Ask more than once, since
        # only a viewer that ignores every attempt is actually wedged.
        deadline = time.time() + timeout
        for _ in range(QUIT_ATTEMPTS):
            if child.poll() is not None:
                break
            try:
                os.write(master, b"\r")  # leave any search or filter prompt
                drain(master, time.time() + 0.1)
                os.write(master, b"q")
            except OSError:
                break
            while time.time() < deadline and child.poll() is None:
                if not drain(master, time.time() + 0.2):
                    break  # nothing more to read; try again or give up

        while time.time() < deadline and child.poll() is None:
            drain(master, time.time() + 0.05)

        if child.poll() is None:
            child.kill()
            child.wait()
            return False, "would not quit after %.0fs" % timeout

        code = child.returncode
        if code < 0:
            name = signal.Signals(-code).name
            return False, "killed by %s" % name
        if code not in (0, 1, 2):
            return False, "exit status %d" % code
        return True, "exit status %d" % code
    finally:
        os.close(master)
        if child.poll() is None:
            child.kill()
            child.wait()


def make_keystrokes(rng, count):
    keys = []
    for _ in range(count):
        roll = rng.random()
        if roll < 0.55:
            keys.append(rng.choice(NAVIGATION))
        elif roll < 0.80:
            keys.append(rng.choice(COMMANDS))
        else:
            # Open a prompt and type something awkward into it. This is the
            # path the original crash lived on.
            keys.append(rng.choice(PROMPTS))
            for _ in range(rng.randint(1, 5)):
                keys.append(rng.choice(NASTY_INPUT))
            keys.append(rng.choice(["\r", "\x1b", "\x7f"]))
    return keys


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("binary", help="path to the csvtui executable")
    parser.add_argument("--rounds", type=int, default=25)
    parser.add_argument("--keys", type=int, default=60,
                        help="keystroke groups per round")
    parser.add_argument("--rows", type=int, default=500)
    parser.add_argument("--seed", type=int, default=None)
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="seconds to wait for a clean exit")
    args = parser.parse_args()

    if not os.access(args.binary, os.X_OK):
        sys.exit("not executable: %s" % args.binary)

    base_seed = args.seed if args.seed is not None else random.randrange(2**31)
    print("csvtui pty fuzz: %d rounds, base seed %d" % (args.rounds, base_seed))

    workdir = tempfile.mkdtemp(prefix="csvtui-fuzz-")
    csv_path = os.path.join(workdir, "fuzz.csv")
    failures = 0

    try:
        for round_index in range(args.rounds):
            seed = base_seed + round_index
            rng = random.Random(seed)
            build_csv(csv_path, args.rows, rng)
            keystrokes = make_keystrokes(rng, args.keys)

            ok, detail = run_round(args.binary, csv_path, rng, keystrokes,
                                   args.timeout)
            if ok:
                print("  round %3d  seed %-12d ok (%s)" % (round_index, seed, detail))
            else:
                failures += 1
                print("  round %3d  seed %-12d FAIL: %s" % (round_index, seed, detail))
                print("    reproduce with: %s --seed %d --rounds 1"
                      % (sys.argv[0], seed))
                print("    keystrokes: %r" % (keystrokes,))
                break
    finally:
        try:
            os.unlink(csv_path)
            os.rmdir(workdir)
        except OSError:
            pass

    if failures:
        print("\n%d round(s) failed" % failures)
        return 1
    print("\nall %d rounds exited cleanly" % args.rounds)
    return 0


if __name__ == "__main__":
    sys.exit(main())
