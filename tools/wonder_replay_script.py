#!/usr/bin/env python3
"""Builds an NX_INPUT_SCRIPT from a recorded play (NX_INPUT_RECORD): the scripted menus of the course runs
(title -> user -> players -> character -> map -> course 1-1) and then the player's own course inputs, shifted so that
they start when the scripted run enters the course.

usage: wonder_replay_script.py <input_record.txt> [--from <s>] [--course-at <s>]
  --from       where the player's course play starts in the recording (default: its first press)
  --course-at  when the scripted run is in the course (default 185)
Prints the script (one line).
"""
import argparse

MENUS = "25:A:0.5;40:A:1.0;60:A:1.0;95:RIGHT:0.3;105:A:1.0;118:A:1.0;150:A:3.0;160:A:0.1;160.5:A:0.1;161:A:0.1;" \
        "161.5:A:0.1;162:A:0.1;170:A:0.05;175:A:0.3;176:A:0.3;177:A:0.3;"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('record')
    ap.add_argument('--from', dest='start', type=float)
    ap.add_argument('--course-at', type=float, default=185.0)
    a = ap.parse_args()
    presses = []
    for item in open(a.record).read().split(';'):
        parts = item.strip().split(':')
        if len(parts) == 3:
            presses.append((float(parts[0]), parts[1], float(parts[2])))
    start = a.start if a.start is not None else min(p[0] for p in presses)
    shift = a.course_at - start
    out = MENUS + ''.join('%.2f:%s:%.2f;' % (t + shift, b, h) for t, b, h in sorted(presses) if t >= start)
    print(out)


if __name__ == '__main__':
    main()
