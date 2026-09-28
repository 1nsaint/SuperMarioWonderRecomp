"""Recompile an NSO module to C++.

Usage: python -m recomp <module.nso> <out dir> [--only 0xADDR ...]

Writes:
  funcs.h         declarations of every recompiled function
  table.cpp       guest offset -> host function table
  imports.cpp     GOT slot -> import name table (resolved by the runtime)
  code_NNN.cpp    the functions, split into translation units
  report.txt      coverage: unhandled instructions, unresolved indirect branches
"""
import argparse
import collections
import os
import time

from . import analysis, emit, nso

INSNS_PER_FILE = 12000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nso")
    ap.add_argument("out")
    ap.add_argument("--only", nargs="*", default=None, help="only these function addresses (debug)")
    args = ap.parse_args()

    t = time.time()
    m = nso.load(args.nso)
    an = analysis.analyze(m)
    print(f"analysis: {len(an.functions)} functions, {len(an.jump_tables)} jump tables ({time.time() - t:.1f}s)")

    os.makedirs(args.out, exist_ok=True)
    e = emit.Emitter(an)
    funcs = an.functions
    if args.only:
        want = {int(x, 16) for x in args.only}
        funcs = [f for f in funcs if f.start in want]

    with open(os.path.join(args.out, "funcs.h"), "w") as h:
        h.write("#pragma once\n#include <nx/cpu.h>\n\n")
        for f in an.functions:
            h.write(f"void {e.fn(f.start)}(nx::CPU& c);\n")

    files = []
    cur, cur_n = [], 0
    for k, f in enumerate(funcs):
        cur.append(e.function(f))
        cur_n += (f.end - f.start) // 4
        if cur_n >= INSNS_PER_FILE or k == len(funcs) - 1:
            name = f"code_{len(files):03d}.cpp"
            with open(os.path.join(args.out, name), "w") as out:
                out.write('#include "funcs.h"\n\n')
                out.write("\n\n".join(cur))
                out.write("\n")
            files.append(name)
            cur, cur_n = [], 0
        if k % 2000 == 0:
            print(f"  emitted {k}/{len(funcs)} functions ({time.time() - t:.0f}s)")

    with open(os.path.join(args.out, "table.cpp"), "w") as out:
        out.write('#include "funcs.h"\n#include <nx/module.h>\n\n')
        out.write("const nx::FuncEntry nx_functions[] = {\n")
        for f in an.functions:
            out.write(f"    {{{f.start:#x}, {e.fn(f.start)}}},\n")
        out.write("};\n")
        out.write(f"const size_t nx_function_count = {len(an.functions)};\n")

    with open(os.path.join(args.out, "imports.cpp"), "w") as out:
        out.write('#include <nx/module.h>\n\n')
        slots = sorted(an.got_imports.items())
        out.write("const nx::ImportSlot nx_imports[] = {\n")
        for got, name in slots:
            out.write(f'    {{{got:#x}, "{name}"}},\n')
        out.write("};\n")
        out.write(f"const size_t nx_import_count = {len(slots)};\n")

    with open(os.path.join(args.out, "report.txt"), "w") as out:
        total = e.handled + sum(e.unhandled.values())
        out.write(f"functions {len(funcs)}, instructions {total}, handled {e.handled}, "
                  f"unhandled {sum(e.unhandled.values())}\n")
        out.write(f"jump tables {len(an.jump_tables)}, unresolved br (indirect tail calls) {len(an.unresolved_br)}\n\n")
        for mn, n in e.unhandled.most_common():
            out.write(f"{n:8} {mn}\n")
    print(f"done: {len(files)} files, handled {e.handled}, unhandled {sum(e.unhandled.values())} "
          f"({time.time() - t:.0f}s)")
    for mn, n in e.unhandled.most_common(30):
        print(f"  {n:7} {mn}")


if __name__ == "__main__":
    main()
