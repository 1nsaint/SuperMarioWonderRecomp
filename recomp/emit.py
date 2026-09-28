"""AArch64 -> C++ instruction emitter.

Each guest function becomes `void sub_XXXXXXXX(nx::CPU& c)`. Registers live in the context; intra-function
branches are gotos, jump tables are switches over the computed target, calls are direct when the target is
known and go through nx::call otherwise. Instructions we do not handle become nx::unimpl traps and are counted.
"""
import collections
import re
import struct

import capstone
from capstone import arm64_const as A

B = "NXB"

CC = {
    A.ARM64_CC_EQ: "c.zf", A.ARM64_CC_NE: "!c.zf", A.ARM64_CC_HS: "c.cf", A.ARM64_CC_LO: "!c.cf",
    A.ARM64_CC_MI: "c.nf", A.ARM64_CC_PL: "!c.nf", A.ARM64_CC_VS: "c.vf", A.ARM64_CC_VC: "!c.vf",
    A.ARM64_CC_HI: "(c.cf && !c.zf)", A.ARM64_CC_LS: "(!c.cf || c.zf)", A.ARM64_CC_GE: "(c.nf == c.vf)",
    A.ARM64_CC_LT: "(c.nf != c.vf)", A.ARM64_CC_GT: "(!c.zf && c.nf == c.vf)",
    A.ARM64_CC_LE: "(c.zf || c.nf != c.vf)", A.ARM64_CC_AL: "true", A.ARM64_CC_NV: "true",
}
INVERT = {A.ARM64_CC_EQ: A.ARM64_CC_NE, A.ARM64_CC_NE: A.ARM64_CC_EQ, A.ARM64_CC_HS: A.ARM64_CC_LO,
          A.ARM64_CC_LO: A.ARM64_CC_HS, A.ARM64_CC_MI: A.ARM64_CC_PL, A.ARM64_CC_PL: A.ARM64_CC_MI,
          A.ARM64_CC_VS: A.ARM64_CC_VC, A.ARM64_CC_VC: A.ARM64_CC_VS, A.ARM64_CC_HI: A.ARM64_CC_LS,
          A.ARM64_CC_LS: A.ARM64_CC_HI, A.ARM64_CC_GE: A.ARM64_CC_LT, A.ARM64_CC_LT: A.ARM64_CC_GE,
          A.ARM64_CC_GT: A.ARM64_CC_LE, A.ARM64_CC_LE: A.ARM64_CC_GT}

VAS = {  # arrangement -> (element bits, lanes)
    A.ARM64_VAS_16B: (8, 16), A.ARM64_VAS_8B: (8, 8), A.ARM64_VAS_8H: (16, 8), A.ARM64_VAS_4H: (16, 4),
    A.ARM64_VAS_4S: (32, 4), A.ARM64_VAS_2S: (32, 2), A.ARM64_VAS_2D: (64, 2), A.ARM64_VAS_1D: (64, 1),
    A.ARM64_VAS_1B: (8, 1), A.ARM64_VAS_1H: (16, 1), A.ARM64_VAS_1S: (32, 1), A.ARM64_VAS_1Q: (128, 1),
    A.ARM64_VAS_4B: (8, 4), A.ARM64_VAS_2H: (16, 2),
}

SYSREG_TPIDR = 0xDE82
SYSREG_TPIDRRO = 0xDE83
SYSREG_FPCR = 0xDA20
SYSREG_FPSR = 0xDA21
SYSREG_NZCV = 0xDA10


class Unhandled(Exception):
    pass


def subst(expr, **vals):
    """Replace the whole-word placeholders a / b / o in an expression template."""
    return re.sub(r"\b([abo])\b", lambda m: vals.get(m.group(1), m.group(1)), expr)


def u(bits):
    return f"uint{bits}_t"


def s(bits):
    return f"int{bits}_t"


class Reg:
    """A register operand: kind 'x' (64-bit gp), 'w' (32-bit gp), 'zr64'/'zr32', or fp/simd b/h/s/d/q/v."""

    def __init__(self, name):
        self.name = name
        if name in ("sp",):
            self.kind, self.n = "x", 31
        elif name == "wsp":
            self.kind, self.n = "w", 31
        elif name == "xzr":
            self.kind, self.n = "zr64", None
        elif name == "wzr":
            self.kind, self.n = "zr32", None
        elif name == "fp":
            self.kind, self.n = "x", 29
        elif name == "lr":
            self.kind, self.n = "x", 30
        elif name[0] in "xwbhsdqv" and name[1:].isdigit():
            self.kind, self.n = name[0], int(name[1:])
        else:
            raise Unhandled(f"register {name}")

    @property
    def gp(self):
        return self.kind in ("x", "w", "zr64", "zr32")

    @property
    def bits(self):
        return {"x": 64, "zr64": 64, "w": 32, "zr32": 32, "b": 8, "h": 16, "s": 32, "d": 64, "q": 128,
                "v": 128}[self.kind]

    @property
    def zr(self):
        return self.kind.startswith("zr")

    def r(self):
        """Read as unsigned of its width."""
        if self.zr:
            return "0ull" if self.bits == 64 else "0u"
        if self.bits == 64:
            return f"c.x[{self.n}]"
        return f"uint32_t(c.x[{self.n}])"

    def w(self, expr):
        if self.zr:
            return f"(void)({expr});"
        if self.bits == 64:
            return f"c.x[{self.n}] = uint64_t({expr});"
        return f"c.x[{self.n}] = uint32_t({expr});"


class Emitter:
    def __init__(self, an, prefix="sub"):
        self.an = an
        self.m = an.module
        self.cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.cs.detail = True
        self.entries = {f.start for f in an.functions}
        self.prefix = prefix
        # Debug: NX_TRACE_SP=0xADDR,... logs SP after every instruction that changes it in those functions.
        import os
        self.trace_sp = {int(x, 16) for x in os.environ.get("NX_TRACE_SP", "").split(",") if x}
        self.unhandled = collections.Counter()
        self.handled = 0

    # ---- helpers ------------------------------------------------------------------------------------------

    def regop(self, i, o):
        return Reg(i.reg_name(o.reg))

    def fn(self, addr):
        return f"{self.prefix}_{addr:08X}"

    def shifted(self, i, o, bits):
        """Value of a register/immediate second operand with shift or extend applied, as uint{bits}."""
        if o.type == A.ARM64_OP_IMM:
            v = o.imm
            if o.shift.type == A.ARM64_SFT_LSL:
                v <<= o.shift.value
            return f"{u(bits)}({v & ((1 << bits) - 1):#x}ull)"
        r = self.regop(i, o)
        v = r.r()
        if o.ext:
            e = o.ext
            src = {A.ARM64_EXT_UXTB: "uint8_t", A.ARM64_EXT_UXTH: "uint16_t", A.ARM64_EXT_UXTW: "uint32_t",
                   A.ARM64_EXT_UXTX: "uint64_t", A.ARM64_EXT_SXTB: "int8_t", A.ARM64_EXT_SXTH: "int16_t",
                   A.ARM64_EXT_SXTW: "int32_t", A.ARM64_EXT_SXTX: "int64_t"}[e]
            v = f"{u(bits)}({s(bits) if src.startswith('int') else u(bits)}({src}({v})))"
            if o.shift.type == A.ARM64_SFT_LSL and o.shift.value:
                v = f"{u(bits)}({v} << {o.shift.value})"
            return v
        if o.shift.type and o.shift.value:
            n = o.shift.value
            t = o.shift.type
            if t == A.ARM64_SFT_LSL:
                return f"{u(bits)}({u(bits)}({v}) << {n})"
            if t == A.ARM64_SFT_LSR:
                return f"{u(bits)}({u(bits)}({v}) >> {n})"
            if t == A.ARM64_SFT_ASR:
                return f"{u(bits)}({s(bits)}({v}) >> {n})"
            if t == A.ARM64_SFT_ROR:
                return f"nx::ror{bits}({v}, {n})"
            raise Unhandled("shift")
        return f"{u(bits)}({v})"

    def cond(self, i):
        return CC[i.cc]

    def target(self, o):
        return o.imm

    def goto_or_tail(self, f, t):
        if f.start <= t < f.end:
            self.labels.add(t)
            return f"goto L_{t:X};"
        if t in self.entries:
            return f"{{ {self.fn(t)}(c); return; }}"
        return f"{{ nx::call(c, {B} + {t:#x}ull); return; }}"

    # ---- memory addressing --------------------------------------------------------------------------------

    def address(self, i, memo, after):
        """Returns (address expression, statement to run after the access for writeback)."""
        base = Reg(i.reg_name(memo.mem.base))
        bexpr = f"c.x[{base.n}]"
        idx = ""
        if memo.mem.index:
            ir = Reg(i.reg_name(memo.mem.index))
            iv = ir.r()
            if memo.ext in (A.ARM64_EXT_SXTW,):
                iv = f"uint64_t(int64_t(int32_t({iv})))"
            elif memo.ext in (A.ARM64_EXT_UXTW,):
                iv = f"uint64_t(uint32_t({iv}))"
            elif memo.ext in (A.ARM64_EXT_SXTX,):
                iv = f"uint64_t({iv})"
            if memo.shift.type == A.ARM64_SFT_LSL and memo.shift.value:
                iv = f"(uint64_t({iv}) << {memo.shift.value})"
            idx = f" + uint64_t({iv})"
        disp = memo.mem.disp
        post = None
        if after is not None:
            if after.type == A.ARM64_OP_IMM:
                post = f"{after.imm & 0xFFFFFFFFFFFFFFFF:#x}ull"
            elif after.type == A.ARM64_OP_REG:
                post = Reg(i.reg_name(after.reg)).r()
        if post is not None:  # post-index (capstone does not always flag writeback for structured ld/st)
            return bexpr + idx, f"c.x[{base.n}] += {post};"
        a = f"{bexpr}{idx} + {disp & 0xFFFFFFFFFFFFFFFF:#x}ull" if disp else bexpr + idx
        if i.writeback:
            return a, f"c.x[{base.n}] = a;"
        return a, ""

    # ---- function ---------------------------------------------------------------------------------------

    def function(self, f):
        self.labels = set()
        body = []
        code = bytes(self.m.image[f.start:f.end])
        insns = {}
        for a in range(f.start, f.end, 4):
            got = list(self.cs.disasm(code[a - f.start:a - f.start + 4], a))
            insns[a] = got[0] if got else None
        jt = self.an.jump_tables
        last_term = False
        for a in range(f.start, f.end, 4):
            i = insns[a]
            if i is None:
                word = struct.unpack_from("<I", code, a - f.start)[0]
                body.append((a, f"nx::unimpl(c, {B} + {a:#x}ull, \"undecodable {word:#010x}\");"))
                last_term = True
                continue
            try:
                stmt = self.insn(f, i, jt)
                self.handled += 1
            except Unhandled as e:
                self.unhandled[i.mnemonic] += 1
                txt = f"{i.mnemonic} {i.op_str}".replace('"', "'")
                stmt = f"nx::unimpl(c, {B} + {a:#x}ull, \"{txt}\"); // {e}"
            if f.start in self.trace_sp and ("sp" in i.op_str.split(",")[0] or "[sp" in i.op_str and i.writeback):
                stmt += f" nx::trace_sp(c, {B} + {a:#x}ull);"
            body.append((a, f"{stmt} // {i.mnemonic} {i.op_str}"))
            last_term = i.mnemonic in ("b", "br", "ret", "udf", "brk") or stmt.endswith("return; }")
        out = [f"void {self.fn(f.start)}(nx::CPU& c) {{"]
        for a, stmt in body:
            if a in self.labels:
                out.append(f"L_{a:X}:;")
            out.append("    " + stmt)
        if not last_term:
            # Falls off the end: continue into whatever follows.
            out.append("    " + (f"{self.fn(f.end)}(c);" if f.end in self.entries else
                                 f"nx::call(c, {B} + {f.end:#x}ull);"))
        out.append("}")
        return "\n".join(out)

    # ---- instruction dispatch -----------------------------------------------------------------------------

    def insn(self, f, i, jt):
        mn = i.mnemonic
        ops = i.operands
        h = getattr(self, "i_" + mn.replace(".", "_"), None)
        if h:
            return h(f, i, ops) if mn not in ("br",) else h(f, i, ops, jt)
        if mn.startswith("b."):
            return f"if ({self.cond(i)}) {self.goto_or_tail(f, self.target(ops[0]))}"
        raise Unhandled("no handler")

    # branches
    def i_b(self, f, i, ops):
        return self.goto_or_tail(f, self.target(ops[0]))

    def i_bl(self, f, i, ops):
        t = self.target(ops[0])
        call = f"{self.fn(t)}(c);" if t in self.entries else f"nx::call(c, {B} + {t:#x}ull);"
        return f"c.x[30] = {B} + {i.address + 4:#x}ull; {call}"

    def i_blr(self, f, i, ops):
        r = self.regop(i, ops[0])
        return f"{{ uint64_t t = {r.r()}; c.x[30] = {B} + {i.address + 4:#x}ull; nx::call(c, t); }}"

    def i_br(self, f, i, ops, jt):
        r = self.regop(i, ops[0])
        if i.address in jt:
            cases = " ".join(f"case {B} + {t:#x}ull: goto L_{t:X};" for t in jt[i.address])
            for t in jt[i.address]:
                self.labels.add(t)
            return f"switch ({r.r()}) {{ {cases} default: nx::call(c, {r.r()}); return; }}"
        return f"{{ nx::call(c, {r.r()}); return; }}"

    def i_ret(self, f, i, ops):
        if ops and self.regop(i, ops[0]).n != 30:
            return f"{{ nx::call(c, {self.regop(i, ops[0]).r()}); return; }}"
        return "return;"

    def i_cbz(self, f, i, ops):
        return f"if ({self.regop(i, ops[0]).r()} == 0) {self.goto_or_tail(f, self.target(ops[1]))}"

    def i_cbnz(self, f, i, ops):
        return f"if ({self.regop(i, ops[0]).r()} != 0) {self.goto_or_tail(f, self.target(ops[1]))}"

    def i_tbz(self, f, i, ops):
        return f"if ((({self.regop(i, ops[0]).r()} >> {ops[1].imm}) & 1) == 0) {self.goto_or_tail(f, self.target(ops[2]))}"

    def i_tbnz(self, f, i, ops):
        return f"if ((({self.regop(i, ops[0]).r()} >> {ops[1].imm}) & 1) != 0) {self.goto_or_tail(f, self.target(ops[2]))}"

    # misc
    def i_nop(self, f, i, ops):
        return ";"

    i_hint = i_nop
    i_prfm = i_nop
    i_prfum = i_nop
    i_yield = i_nop

    i_wfe = i_nop
    i_wfi = i_nop
    i_sev = i_nop
    i_sevl = i_nop
    def i_dc(self, f, i, ops):
        if "zva" in i.op_str:  # zero a 64-byte block (DCZID_EL0 reports 64)
            r = Reg(i.op_str.split(",")[1].strip())
            return f"std::memset(reinterpret_cast<void*>(c.x[{r.n}] & ~63ull), 0, 64);"
        return ";"  # cache maintenance

    i_ic = i_nop
    i_autia1716 = i_nop
    i_autib1716 = i_nop
    i_pacia1716 = i_nop
    i_pacib1716 = i_nop

    def crc(self, i, ops, poly, size):
        d, n, m = (self.regop(i, o) for o in ops[:3])
        return d.w(f"nx::crc32<{size}>({n.r()}, {m.r()}, {poly:#x}u)")

    def i_crc32b(self, f, i, ops):
        return self.crc(i, ops, 0xEDB88320, 1)

    def i_crc32h(self, f, i, ops):
        return self.crc(i, ops, 0xEDB88320, 2)

    def i_crc32w(self, f, i, ops):
        return self.crc(i, ops, 0xEDB88320, 4)

    def i_crc32x(self, f, i, ops):
        return self.crc(i, ops, 0xEDB88320, 8)

    def i_crc32cb(self, f, i, ops):
        return self.crc(i, ops, 0x82F63B78, 1)

    def i_crc32ch(self, f, i, ops):
        return self.crc(i, ops, 0x82F63B78, 2)

    def i_crc32cw(self, f, i, ops):
        return self.crc(i, ops, 0x82F63B78, 4)

    def i_crc32cx(self, f, i, ops):
        return self.crc(i, ops, 0x82F63B78, 8)

    def i_ngc(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"{u(d.bits)}({u(d.bits)}(~{self.regop(i, ops[1]).r()}) + c.cf)")

    def i_ngcs(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"nx::adds{d.bits}(c, 0, {u(d.bits)}(~{self.regop(i, ops[1]).r()}), c.cf)")

    def i_dmb(self, f, i, ops):
        return "nx::barrier();"

    i_dsb = i_dmb
    i_isb = i_nop

    def i_clrex(self, f, i, ops):
        return "nx::clrex(c);"

    def i_udf(self, f, i, ops):
        return f"nx::unimpl(c, {B} + {i.address:#x}ull, \"udf\");"

    def i_brk(self, f, i, ops):
        return f"nx::unimpl(c, {B} + {i.address:#x}ull, \"brk\");"

    def i_svc(self, f, i, ops):
        return f"nx::svc(c, {ops[0].imm}, {B} + {i.address:#x}ull);"

    def sysreg(self, i):
        word = struct.unpack("<I", bytes(i.bytes))[0]
        return (word >> 5) & 0xFFFF

    def i_mrs(self, f, i, ops):
        d = self.regop(i, ops[0])
        sr = self.sysreg(i)
        v = {SYSREG_TPIDR: "c.tpidr", SYSREG_TPIDRRO: "c.tpidrro", SYSREG_FPCR: "c.fpcr", SYSREG_FPSR: "c.fpsr",
             SYSREG_NZCV: "nx::get_nzcv(c)"}.get(sr, f"nx::mrs(c, {sr:#x})")
        return d.w(v)

    def i_msr(self, f, i, ops):
        word = struct.unpack("<I", bytes(i.bytes))[0]
        if (word >> 20) != 0xD51:
            raise Unhandled("msr pstate")
        sr = self.sysreg(i)
        v = Reg(i.reg_name(ops[-1].reg)).r()
        return {SYSREG_TPIDR: f"c.tpidr = {v};", SYSREG_FPCR: f"c.fpcr = {v};", SYSREG_FPSR: f"c.fpsr = {v};",
                SYSREG_NZCV: f"nx::set_nzcv(c, uint32_t({v} >> 28));"}.get(sr, f"nx::msr(c, {sr:#x}, {v});")

    # ---- integer data processing --------------------------------------------------------------------------

    def i_mov(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp or (ops[1].type == A.ARM64_OP_REG and not self.regop(i, ops[1]).gp):
            return self.v_mov(f, i, ops)
        if ops[1].type == A.ARM64_OP_IMM:
            return d.w(f"{ops[1].imm & ((1 << d.bits) - 1):#x}ull")
        return d.w(self.regop(i, ops[1]).r())

    def i_movz(self, f, i, ops):
        d = self.regop(i, ops[0])
        v = ops[1].imm << (ops[1].shift.value if ops[1].shift.type else 0)
        return d.w(f"{v & ((1 << d.bits) - 1):#x}ull")

    def i_movn(self, f, i, ops):
        d = self.regop(i, ops[0])
        v = ~(ops[1].imm << (ops[1].shift.value if ops[1].shift.type else 0))
        return d.w(f"{v & ((1 << d.bits) - 1):#x}ull")

    def i_movk(self, f, i, ops):
        d = self.regop(i, ops[0])
        sh = ops[1].shift.value if ops[1].shift.type else 0
        mask = (0xFFFF << sh) & ((1 << d.bits) - 1)
        return d.w(f"({d.r()} & {~mask & ((1 << d.bits) - 1):#x}ull) | {(ops[1].imm & 0xFFFF) << sh:#x}ull")

    def i_adrp(self, f, i, ops):
        return self.regop(i, ops[0]).w(f"{B} + {ops[1].imm:#x}ull")

    i_adr = i_adrp

    def arith(self, i, ops, op, setflags):
        d = self.regop(i, ops[0])
        bits = 64 if d.bits == 64 else 32
        n = self.regop(i, ops[1])
        a = n.r()
        b = self.shifted(i, ops[2], bits)
        if setflags:
            fn = "adds" if op == "+" else "subs"
            return d.w(f"nx::{fn}{bits}(c, {a}, {b})" if op == "-" else f"nx::adds{bits}(c, {a}, {b}, 0)")
        return d.w(f"{u(bits)}({a}) {op} {b}")

    def i_add(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            return self.v_bin(i, ops, "a + b")
        return self.arith(i, ops, "+", False)

    def i_sub(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            return self.v_bin(i, ops, "a - b")
        return self.arith(i, ops, "-", False)

    def i_adds(self, f, i, ops):
        return self.arith(i, ops, "+", True)

    def i_subs(self, f, i, ops):
        return self.arith(i, ops, "-", True)

    def i_cmp(self, f, i, ops):
        n = self.regop(i, ops[0])
        bits = 64 if n.bits == 64 else 32
        return f"nx::subs{bits}(c, {n.r()}, {self.shifted(i, ops[1], bits)});"

    def i_cmn(self, f, i, ops):
        n = self.regop(i, ops[0])
        bits = 64 if n.bits == 64 else 32
        return f"nx::adds{bits}(c, {n.r()}, {self.shifted(i, ops[1], bits)}, 0);"

    def i_neg(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp:
            return self.v_un(i, ops, "-a", signed=True)
        bits = d.bits
        return d.w(f"{u(bits)}(0) - {self.shifted(i, ops[1], bits)}")

    def i_negs(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"nx::subs{d.bits}(c, 0, {self.shifted(i, ops[1], d.bits)})")

    def i_adc(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"{u(d.bits)}({self.regop(i, ops[1]).r()} + {self.regop(i, ops[2]).r()} + c.cf)")

    def i_adcs(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"nx::adds{d.bits}(c, {self.regop(i, ops[1]).r()}, {self.regop(i, ops[2]).r()}, c.cf)")

    def i_sbc(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"{u(d.bits)}({self.regop(i, ops[1]).r()} + {u(d.bits)}(~{self.regop(i, ops[2]).r()}) + c.cf)")

    def i_sbcs(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"nx::adds{d.bits}(c, {self.regop(i, ops[1]).r()}, {u(d.bits)}(~{self.regop(i, ops[2]).r()}), c.cf)")

    def logic(self, i, ops, expr, setflags=False, invert=False):
        d = self.regop(i, ops[0])
        if not d.gp:
            raise Unhandled("vector logic here")
        bits = d.bits
        a = self.regop(i, ops[1]).r()
        b = self.shifted(i, ops[2], bits)
        if invert:
            b = f"{u(bits)}(~{b})"
        v = f"{u(bits)}({expr.format(a=a, b=b)})"
        if setflags:
            return f"{{ {u(bits)} r = {v}; nx::logic{bits}(c, r); {d.w('r')} }}"
        return d.w(v)

    def i_and(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            return self.v_bin(i, ops, "a & b", lanes="bytes")
        return self.logic(i, ops, "{a} & {b}")

    def i_ands(self, f, i, ops):
        return self.logic(i, ops, "{a} & {b}", setflags=True)

    def i_orr(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            if ops[1].type == A.ARM64_OP_IMM:
                return self.v_imm_logic(i, ops, "|")
            return self.v_bin(i, ops, "a | b", lanes="bytes")
        return self.logic(i, ops, "{a} | {b}")

    def i_eor(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            return self.v_bin(i, ops, "a ^ b", lanes="bytes")
        return self.logic(i, ops, "{a} ^ {b}")

    def i_bic(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            if ops[1].type == A.ARM64_OP_IMM:
                return self.v_imm_logic(i, ops, "&~")
            return self.v_bin(i, ops, "a & ~b", lanes="bytes")
        return self.logic(i, ops, "{a} & {b}", invert=True)

    def i_bics(self, f, i, ops):
        return self.logic(i, ops, "{a} & {b}", setflags=True, invert=True)

    def i_orn(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            return self.v_bin(i, ops, "a | ~b", lanes="bytes")
        return self.logic(i, ops, "{a} | {b}", invert=True)

    def i_eon(self, f, i, ops):
        return self.logic(i, ops, "{a} ^ {b}", invert=True)

    def i_tst(self, f, i, ops):
        n = self.regop(i, ops[0])
        bits = n.bits
        return f"nx::logic{bits}(c, {u(bits)}({n.r()} & {self.shifted(i, ops[1], bits)}));"

    def i_mvn(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp:
            return self.v_un(i, ops, "~a", lanes="bytes")
        return d.w(f"{u(d.bits)}(~{self.shifted(i, ops[1], d.bits)})")

    def shift_op(self, i, ops, kind):
        d = self.regop(i, ops[0])
        bits = d.bits
        a = self.regop(i, ops[1]).r()
        if ops[2].type == A.ARM64_OP_IMM:
            n = str(ops[2].imm)
        else:
            n = f"({self.regop(i, ops[2]).r()} & {bits - 1})"
        return d.w({"lsl": f"{u(bits)}({a}) << {n}", "lsr": f"{u(bits)}({a}) >> {n}",
                    "asr": f"nx::asr{bits}({a}, unsigned({n}))", "ror": f"nx::ror{bits}({a}, unsigned({n}))"}[kind])

    def i_lsl(self, f, i, ops):
        return self.shift_op(i, ops, "lsl")

    def i_lsr(self, f, i, ops):
        return self.shift_op(i, ops, "lsr")

    def i_asr(self, f, i, ops):
        return self.shift_op(i, ops, "asr")

    def i_ror(self, f, i, ops):
        return self.shift_op(i, ops, "ror")

    def i_extr(self, f, i, ops):
        d = self.regop(i, ops[0])
        bits = d.bits
        hi, lo, n = self.regop(i, ops[1]).r(), self.regop(i, ops[2]).r(), ops[3].imm
        if n == 0:
            return d.w(lo)
        return d.w(f"({u(bits)}({lo}) >> {n}) | ({u(bits)}({hi}) << {bits - n})")

    def bitfield(self, i, ops, lsb, width, signed, insert=False, to_pos=None):
        """Extract `width` bits at `lsb` from source, place at `to_pos` (0 = extract)."""
        d = self.regop(i, ops[0])
        bits = d.bits
        src = self.regop(i, ops[1]).r()
        field_mask = (1 << width) - 1
        if signed:
            v = f"{u(bits)}({s(bits)}({u(bits)}({src}) << {bits - lsb - width}) >> {bits - width})"
        else:
            v = f"(({u(bits)}({src}) >> {lsb}) & {field_mask:#x}ull)"
        pos = to_pos or 0
        if pos:
            v = f"{u(bits)}({v} << {pos})"
        if insert:
            keep = ~(field_mask << pos) & ((1 << bits) - 1)
            v = f"({d.r()} & {keep:#x}ull) | ({v} & {(field_mask << pos) & ((1 << bits) - 1):#x}ull)"
        return d.w(v)

    def i_ubfx(self, f, i, ops):
        return self.bitfield(i, ops, ops[2].imm, ops[3].imm, False)

    def i_sbfx(self, f, i, ops):
        return self.bitfield(i, ops, ops[2].imm, ops[3].imm, True)

    def i_ubfiz(self, f, i, ops):
        return self.bitfield(i, ops, 0, ops[3].imm, False, to_pos=ops[2].imm)

    def i_sbfiz(self, f, i, ops):
        d = self.regop(i, ops[0])
        bits, lsb, width = d.bits, ops[2].imm, ops[3].imm
        src = self.regop(i, ops[1]).r()
        return d.w(f"{u(bits)}({s(bits)}({u(bits)}({src}) << {bits - width}) >> {bits - width - lsb})")

    def i_bfi(self, f, i, ops):
        return self.bitfield(i, ops, 0, ops[3].imm, False, insert=True, to_pos=ops[2].imm)

    def i_bfxil(self, f, i, ops):
        return self.bitfield(i, ops, ops[2].imm, ops[3].imm, False, insert=True)

    def i_ubfm(self, f, i, ops):
        raise Unhandled("raw ubfm")

    def ext(self, i, ops, t):
        d = self.regop(i, ops[0])
        src = self.regop(i, ops[1]).r()
        return d.w(f"{u(d.bits)}({s(d.bits)}({t}({src})))" if t.startswith("int") else f"{u(d.bits)}({t}({src}))")

    def i_sxtb(self, f, i, ops):
        return self.ext(i, ops, "int8_t")

    def i_sxth(self, f, i, ops):
        return self.ext(i, ops, "int16_t")

    def i_sxtw(self, f, i, ops):
        return self.ext(i, ops, "int32_t")

    def i_uxtb(self, f, i, ops):
        return self.ext(i, ops, "uint8_t")

    def i_uxth(self, f, i, ops):
        return self.ext(i, ops, "uint16_t")

    def i_csel(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"{self.cond(i)} ? {self.regop(i, ops[1]).r()} : {self.regop(i, ops[2]).r()}")

    def i_csinc(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"{self.cond(i)} ? {u(d.bits)}({self.regop(i, ops[1]).r()}) : {u(d.bits)}({self.regop(i, ops[2]).r()} + 1)")

    def i_csinv(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"{self.cond(i)} ? {u(d.bits)}({self.regop(i, ops[1]).r()}) : {u(d.bits)}(~{self.regop(i, ops[2]).r()})")

    def i_csneg(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"{self.cond(i)} ? {u(d.bits)}({self.regop(i, ops[1]).r()}) : {u(d.bits)}(0 - {self.regop(i, ops[2]).r()})")

    def i_cset(self, f, i, ops):
        return self.regop(i, ops[0]).w(f"({self.cond(i)}) ? 1 : 0")

    def i_csetm(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"({self.cond(i)}) ? {u(d.bits)}(~{u(d.bits)}(0)) : {u(d.bits)}(0)")

    def i_cinc(self, f, i, ops):
        d = self.regop(i, ops[0])
        n = self.regop(i, ops[1]).r()
        return d.w(f"({self.cond(i)}) ? {u(d.bits)}({n} + 1) : {u(d.bits)}({n})")

    def i_cinv(self, f, i, ops):
        d = self.regop(i, ops[0])
        n = self.regop(i, ops[1]).r()
        return d.w(f"({self.cond(i)}) ? {u(d.bits)}(~{n}) : {u(d.bits)}({n})")

    def i_cneg(self, f, i, ops):
        d = self.regop(i, ops[0])
        n = self.regop(i, ops[1]).r()
        return d.w(f"({self.cond(i)}) ? {u(d.bits)}(0 - {n}) : {u(d.bits)}({n})")

    def ccmp(self, i, ops, fn):
        n = self.regop(i, ops[0])
        bits = n.bits
        b = self.shifted(i, ops[1], bits)
        return f"if ({self.cond(i)}) nx::{fn}{bits}(c, {n.r()}, {b}{', 0' if fn == 'adds' else ''}); else nx::set_nzcv(c, {ops[2].imm});"

    def i_ccmp(self, f, i, ops):
        return self.ccmp(i, ops, "subs")

    def i_ccmn(self, f, i, ops):
        return self.ccmp(i, ops, "adds")

    def i_madd(self, f, i, ops):
        d = self.regop(i, ops[0])
        bits = d.bits
        n, m, a = (self.regop(i, o).r() for o in ops[1:4])
        return d.w(f"{u(bits)}({a} + {u(bits)}({n}) * {u(bits)}({m}))")

    def i_msub(self, f, i, ops):
        d = self.regop(i, ops[0])
        bits = d.bits
        n, m, a = (self.regop(i, o).r() for o in ops[1:4])
        return d.w(f"{u(bits)}({a} - {u(bits)}({n}) * {u(bits)}({m}))")

    def i_mul(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp:
            return self.v_bin(i, ops, "a * b")
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        return d.w(f"{u(d.bits)}({u(d.bits)}({n}) * {u(d.bits)}({m}))")

    def i_mneg(self, f, i, ops):
        d = self.regop(i, ops[0])
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        return d.w(f"{u(d.bits)}(0 - {u(d.bits)}({n}) * {u(d.bits)}({m}))")

    def long_mul(self, i, ops, signed, acc=None):
        d = self.regop(i, ops[0])
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        t = "int64_t(int32_t" if signed else "uint64_t(uint32_t"
        prod = f"uint64_t({t}({n})) * {t}({m})))"
        if acc is None:
            return d.w(prod)
        a = self.regop(i, ops[3]).r()
        return d.w(f"{a} {acc} {prod}")

    def i_smull(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            return self.v_long(i, ops, "a * b", signed=True, high=False)
        return self.long_mul(i, ops, True)

    def i_umull(self, f, i, ops):
        if not self.regop(i, ops[0]).gp:
            return self.v_long(i, ops, "a * b", signed=False, high=False)
        return self.long_mul(i, ops, False)

    def i_smaddl(self, f, i, ops):
        return self.long_mul(i, ops, True, "+")

    def i_umaddl(self, f, i, ops):
        return self.long_mul(i, ops, False, "+")

    def i_smsubl(self, f, i, ops):
        return self.long_mul(i, ops, True, "-")

    def i_umsubl(self, f, i, ops):
        return self.long_mul(i, ops, False, "-")

    def i_smulh(self, f, i, ops):
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        return self.regop(i, ops[0]).w(f"uint64_t(nx::smulh(int64_t({n}), int64_t({m})))")

    def i_umulh(self, f, i, ops):
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        return self.regop(i, ops[0]).w(f"nx::umulh({n}, {m})")

    def i_udiv(self, f, i, ops):
        d = self.regop(i, ops[0])
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        return d.w(f"nx::udiv{d.bits}({n}, {m})")

    def i_sdiv(self, f, i, ops):
        d = self.regop(i, ops[0])
        n, m = (self.regop(i, o).r() for o in ops[1:3])
        return d.w(f"{u(d.bits)}(nx::sdiv{d.bits}({s(d.bits)}({n}), {s(d.bits)}({m})))")

    def i_clz(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"std::countl_zero({u(d.bits)}({self.regop(i, ops[1]).r()}))")

    def i_cls(self, f, i, ops):
        d = self.regop(i, ops[0])
        n = self.regop(i, ops[1]).r()
        return d.w(f"std::countl_zero({u(d.bits)}(({u(d.bits)}({n}) ^ ({u(d.bits)}({n}) << 1)) | 1)) ")

    def i_rbit(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"nx::rbit{d.bits}({self.regop(i, ops[1]).r()})")

    def i_rev(self, f, i, ops):
        d = self.regop(i, ops[0])
        return d.w(f"nx::bswap({u(d.bits)}({self.regop(i, ops[1]).r()}))")

    def i_rev16(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp:
            return self.v_rev(i, ops, 16)
        return d.w(f"nx::rev16_{d.bits}({self.regop(i, ops[1]).r()})")

    def i_rev32(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not d.gp:
            return self.v_rev(i, ops, 32)
        return d.w(f"nx::rev32_64({self.regop(i, ops[1]).r()})")

    def i_rev64(self, f, i, ops):
        return self.v_rev(i, ops, 64)

    # ---- loads / stores -----------------------------------------------------------------------------------

    def mem_ops(self, ops):
        """(register operands, mem operand, trailing post-index operand)."""
        regs, mem, after = [], None, None
        for o in ops:
            if o.type == A.ARM64_OP_MEM:
                mem = o
            elif mem is None:
                regs.append(o)
            else:
                after = o
        return regs, mem, after

    def load_into(self, r, addr, size, signed):
        if r.gp:
            if signed:
                return r.w(f"{u(r.bits)}({s(r.bits)}(nx::ld<{s(size * 8)}>({addr})))")
            return r.w(f"nx::ld<{u(size * 8)}>({addr})")
        if size == 16:
            return f"c.v[{r.n}] = nx::ld<nx::V128>({addr});"
        return f"{{ nx::V128 t_ = nx::vzero(); t_.u{size * 8}[0] = nx::ld<{u(size * 8)}>({addr}); c.v[{r.n}] = t_; }}"

    def store_from(self, r, addr, size):
        if r.gp:
            return f"nx::st<{u(size * 8)}>({addr}, {u(size * 8)}({r.r()}));"
        if size == 16:
            return f"nx::st<nx::V128>({addr}, c.v[{r.n}]);"
        return f"nx::st<{u(size * 8)}>({addr}, c.v[{r.n}].u{size * 8}[0]);"

    def ldst(self, i, ops, size=None, signed=False, store=False, pair=False):
        regs, mem, after = self.mem_ops(ops)
        rs = [self.regop(i, o) for o in regs]
        if mem is None:
            # Literal: ldr x0, #addr
            addr = f"{B} + {regs[-1].imm:#x}ull" if False else f"{B} + {ops[-1].imm:#x}ull"
            r = self.regop(i, ops[0])
            sz = size or r.bits // 8
            return self.load_into(r, addr, sz, signed)
        sz = size or rs[0].bits // 8
        a, wb = self.address(i, mem, after)
        parts = [f"uint64_t a = {a};"]
        if store:
            for k, r in enumerate(rs):
                parts.append(self.store_from(r, f"a + {k * sz}" if k else "a", sz))
            parts.append(wb)
        else:
            # Load everything first so base-register overlap cannot corrupt the address.
            if pair:
                tmp = []
                for k, r in enumerate(rs):
                    parts.append(f"auto v{k} = nx::ld<{'nx::V128' if sz == 16 else (s(sz * 8) if signed else u(sz * 8))}>(a + {k * sz});")
                    tmp.append(k)
                parts.append(wb)
                for k, r in enumerate(rs):
                    if r.gp:
                        parts.append(r.w(f"{u(r.bits)}({s(r.bits)}(v{k}))" if signed else f"v{k}"))
                    elif sz == 16:
                        parts.append(f"c.v[{r.n}] = v{k};")
                    else:
                        parts.append(f"{{ nx::V128 t_ = nx::vzero(); t_.u{sz * 8}[0] = v{k}; c.v[{r.n}] = t_; }}")
            else:
                parts.append(wb)
                parts.append(self.load_into(rs[0], "a", sz, signed))
                if wb and rs[0].gp and rs[0].n == Reg(i.reg_name(mem.mem.base)).n:
                    raise Unhandled("load writeback to base")
        return "{ " + " ".join(p for p in parts if p) + " }"

    def i_ldr(self, f, i, ops):
        return self.ldst(i, ops)

    i_ldur = i_ldr

    def i_ldrb(self, f, i, ops):
        return self.ldst(i, ops, 1)

    i_ldurb = i_ldrb

    def i_ldrh(self, f, i, ops):
        return self.ldst(i, ops, 2)

    i_ldurh = i_ldrh

    def i_ldrsb(self, f, i, ops):
        return self.ldst(i, ops, 1, signed=True)

    i_ldursb = i_ldrsb

    def i_ldrsh(self, f, i, ops):
        return self.ldst(i, ops, 2, signed=True)

    i_ldursh = i_ldrsh

    def i_ldrsw(self, f, i, ops):
        return self.ldst(i, ops, 4, signed=True)

    i_ldursw = i_ldrsw

    def i_str(self, f, i, ops):
        return self.ldst(i, ops, store=True)

    i_stur = i_str

    def i_strb(self, f, i, ops):
        return self.ldst(i, ops, 1, store=True)

    i_sturb = i_strb

    def i_strh(self, f, i, ops):
        return self.ldst(i, ops, 2, store=True)

    i_sturh = i_strh

    def i_ldp(self, f, i, ops):
        return self.ldst(i, ops, pair=True)

    i_ldnp = i_ldp

    def i_ldpsw(self, f, i, ops):
        return self.ldst(i, ops, 4, signed=True, pair=True)

    def i_stp(self, f, i, ops):
        return self.ldst(i, ops, store=True)

    i_stnp = i_stp

    # acquire / release / exclusive
    def acq(self, i, ops, size):
        regs, mem, _ = self.mem_ops(ops)
        d = self.regop(i, regs[0])
        sz = size or d.bits // 8
        return d.w(f"nx::ld_acq<{u(sz * 8)}>(c.x[{Reg(i.reg_name(mem.mem.base)).n}])")

    def rel(self, i, ops, size):
        regs, mem, _ = self.mem_ops(ops)
        r = self.regop(i, regs[0])
        sz = size or r.bits // 8
        return f"nx::st_rel<{u(sz * 8)}>(c.x[{Reg(i.reg_name(mem.mem.base)).n}], {u(sz * 8)}({r.r()}));"

    def i_ldar(self, f, i, ops):
        return self.acq(i, ops, None)

    def i_ldarb(self, f, i, ops):
        return self.acq(i, ops, 1)

    def i_ldarh(self, f, i, ops):
        return self.acq(i, ops, 2)

    def i_stlr(self, f, i, ops):
        return self.rel(i, ops, None)

    def i_stlrb(self, f, i, ops):
        return self.rel(i, ops, 1)

    def i_stlrh(self, f, i, ops):
        return self.rel(i, ops, 2)

    def ldx(self, i, ops, size):
        regs, mem, _ = self.mem_ops(ops)
        d = self.regop(i, regs[0])
        sz = size or d.bits // 8
        return d.w(f"nx::ldxr<{u(sz * 8)}>(c, c.x[{Reg(i.reg_name(mem.mem.base)).n}])")

    def stx(self, i, ops, size):
        regs, mem, _ = self.mem_ops(ops)
        st, r = self.regop(i, regs[0]), self.regop(i, regs[1])
        sz = size or r.bits // 8
        return st.w(f"nx::stxr<{u(sz * 8)}>(c, c.x[{Reg(i.reg_name(mem.mem.base)).n}], {u(sz * 8)}({r.r()}))")

    def i_ldaxr(self, f, i, ops):
        return self.ldx(i, ops, None)

    i_ldxr = i_ldaxr

    def i_ldaxrb(self, f, i, ops):
        return self.ldx(i, ops, 1)

    i_ldxrb = i_ldaxrb

    def i_ldaxrh(self, f, i, ops):
        return self.ldx(i, ops, 2)

    i_ldxrh = i_ldaxrh

    def i_stlxr(self, f, i, ops):
        return self.stx(i, ops, None)

    i_stxr = i_stlxr

    def i_stlxrb(self, f, i, ops):
        return self.stx(i, ops, 1)

    i_stxrb = i_stlxrb

    def i_stlxrh(self, f, i, ops):
        return self.stx(i, ops, 2)

    i_stxrh = i_stlxrh

    # ---- scalar floating point ----------------------------------------------------------------------------

    def fr(self, r):
        """Read a scalar FP register as float/double."""
        if r.kind == "s":
            return f"c.v[{r.n}].f32[0]"
        if r.kind == "d":
            return f"c.v[{r.n}].f64[0]"
        if r.kind == "h":
            return f"nx::f16_to_f32(c.v[{r.n}].u16[0])"
        raise Unhandled(f"fp read {r.name}")

    def fw(self, r, expr):
        """Write a scalar FP register (upper bits cleared)."""
        if r.kind == "h":
            return f"{{ uint16_t h_ = nx::f32_to_f16(float({expr})); nx::V128 t_ = nx::vzero(); t_.u16[0] = h_; c.v[{r.n}] = t_; }}"
        field = {"s": "f32", "d": "f64"}.get(r.kind)
        if not field:
            raise Unhandled(f"fp write {r.name}")
        ty = "float" if r.kind == "s" else "double"
        return f"{{ {ty} r_ = {ty}({expr}); nx::V128 t_ = nx::vzero(); t_.{field}[0] = r_; c.v[{r.n}] = t_; }}"

    def is_vec(self, i, o):
        return o.type == A.ARM64_OP_REG and (o.vas or o.vector_index != -1) and not self.regop(i, o).gp

    def fbin(self, i, ops, expr):
        if self.is_vec(i, ops[0]):
            return self.v_bin(i, ops, expr, fp=True)
        d, n, m = (self.regop(i, o) for o in ops[:3])
        if self.is_vec(i, ops[2]):  # scalar by element: fmul s0, s1, v2.s[1]
            m_expr = self.velem(i, ops[2], "f")
        else:
            m_expr = self.fr(m)
        return self.fw(d, subst(expr, a=f"({self.fr(n)})", b=f"({m_expr})"))

    def velem(self, i, o, kind):
        r = self.regop(i, o)
        bits = VAS[o.vas][0] if o.vas else {"b": 8, "h": 16, "s": 32, "d": 64}[r.kind]
        return f"c.v[{r.n}].{kind}{bits}[{o.vector_index}]"

    def i_fadd(self, f, i, ops):
        return self.fbin(i, ops, "a + b")

    def i_fsub(self, f, i, ops):
        return self.fbin(i, ops, "a - b")

    def i_fmul(self, f, i, ops):
        return self.fbin(i, ops, "a * b")

    def i_fdiv(self, f, i, ops):
        return self.fbin(i, ops, "a / b")

    def i_fnmul(self, f, i, ops):
        return self.fbin(i, ops, "-(a * b)")

    def i_fabd(self, f, i, ops):
        return self.fbin(i, ops, "std::fabs(a - b)")

    def i_fmaxnm(self, f, i, ops):
        return self.fbin(i, ops, "nx::fmaxnm(a, b)")

    def i_fminnm(self, f, i, ops):
        return self.fbin(i, ops, "nx::fminnm(a, b)")

    def i_fmax(self, f, i, ops):
        return self.fbin(i, ops, "nx::fmax_(a, b)")

    def i_fmin(self, f, i, ops):
        return self.fbin(i, ops, "nx::fmin_(a, b)")

    def fma(self, i, ops, kind):
        d, n, m, a = (self.regop(i, o) for o in ops[:4])
        N, M, Aa = self.fr(n), self.fr(m), self.fr(a)
        e = {"fmadd": f"std::fma({N}, {M}, {Aa})", "fmsub": f"std::fma(-({N}), {M}, {Aa})",
             "fnmadd": f"std::fma(-({N}), {M}, -({Aa}))", "fnmsub": f"std::fma({N}, {M}, -({Aa}))"}[kind]
        return self.fw(d, e)

    def i_fmadd(self, f, i, ops):
        return self.fma(i, ops, "fmadd")

    def i_fmsub(self, f, i, ops):
        return self.fma(i, ops, "fmsub")

    def i_fnmadd(self, f, i, ops):
        return self.fma(i, ops, "fnmadd")

    def i_fnmsub(self, f, i, ops):
        return self.fma(i, ops, "fnmsub")

    def fun(self, i, ops, expr):
        if self.is_vec(i, ops[0]):
            return self.v_un(i, ops, expr, fp=True)
        d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
        return self.fw(d, subst(expr, a=f"({self.fr(n)})"))

    def i_fneg(self, f, i, ops):
        return self.fun(i, ops, "-a")

    def i_fabs(self, f, i, ops):
        return self.fun(i, ops, "std::fabs(a)")

    def i_fsqrt(self, f, i, ops):
        return self.fun(i, ops, "std::sqrt(a)")

    def i_frintm(self, f, i, ops):
        return self.fun(i, ops, "std::floor(a)")

    def i_frintp(self, f, i, ops):
        return self.fun(i, ops, "std::ceil(a)")

    def i_frintz(self, f, i, ops):
        return self.fun(i, ops, "std::trunc(a)")

    def i_frinta(self, f, i, ops):
        return self.fun(i, ops, "nx::round_away(a)")

    def i_frintn(self, f, i, ops):
        return self.fun(i, ops, "nx::round_even(a)")

    i_frintx = i_frintn
    i_frinti = i_frintn

    def i_fcmp(self, f, i, ops):
        n = self.regop(i, ops[0])
        b = "0.0" if ops[1].type == A.ARM64_OP_FP or (ops[1].type == A.ARM64_OP_IMM) else self.fr(self.regop(i, ops[1]))
        ty = "float" if n.kind in ("s", "h") else "double"
        return f"nx::fcmp<{ty}>(c, {self.fr(n)}, {ty}({b}));"

    i_fcmpe = i_fcmp

    def i_fccmp(self, f, i, ops):
        n, m = self.regop(i, ops[0]), self.regop(i, ops[1])
        ty = "float" if n.kind in ("s", "h") else "double"
        return f"if ({self.cond(i)}) nx::fcmp<{ty}>(c, {self.fr(n)}, {self.fr(m)}); else nx::set_nzcv(c, {ops[2].imm});"

    i_fccmpe = i_fccmp

    def i_fcsel(self, f, i, ops):
        d, n, m = (self.regop(i, o) for o in ops[:3])
        return self.fw(d, f"({self.cond(i)}) ? {self.fr(n)} : {self.fr(m)}")

    def i_fmov(self, f, i, ops):
        d = self.regop(i, ops[0])
        o = ops[1]
        if self.is_vec(i, ops[0]) and ops[0].vector_index != -1:  # fmov v0.d[1], x1
            return f"c.v[{d.n}].u64[{ops[0].vector_index}] = {self.regop(i, o).r()};"
        if o.type == A.ARM64_OP_FP:
            if self.is_vec(i, ops[0]):
                bits, lanes = VAS[ops[0].vas]
                t = "f32" if bits == 32 else "f64"
                return f"{{ nx::V128 r = nx::vzero(); for (int k = 0; k < {lanes}; k++) r.{t}[k] = {o.fp!r}; c.v[{d.n}] = r; }}"
            return self.fw(d, repr(o.fp))
        if o.type == A.ARM64_OP_REG:
            n = self.regop(i, o)
            if self.is_vec(i, o) and o.vector_index != -1:  # fmov x0, v1.d[1]
                return d.w(f"c.v[{n.n}].u64[{o.vector_index}]")
            if d.gp and not n.gp:
                return d.w(f"c.v[{n.n}].u{d.bits}[0]")
            if not d.gp and n.gp:
                return f"{{ nx::V128 t_ = nx::vzero(); t_.u{d.bits}[0] = {u(d.bits)}({n.r()}); c.v[{d.n}] = t_; }}"
            return f"{{ nx::V128 t_ = nx::vzero(); t_.u{d.bits}[0] = c.v[{n.n}].u{d.bits}[0]; c.v[{d.n}] = t_; }}"
        if o.type == A.ARM64_OP_IMM:  # fmov s0, #0? capstone sometimes gives imm
            return self.fw(d, "0.0")
        raise Unhandled("fmov form")

    def i_fcvt(self, f, i, ops):
        d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
        return self.fw(d, self.fr(n))

    def cvt_to_int(self, i, ops, rnd, signed):
        if self.is_vec(i, ops[0]):
            return self.v_cvt_to_int(i, ops, rnd, signed)
        d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
        if not d.gp:
            raise Unhandled("fcvt to simd scalar")
        v = self.fr(n)
        if len(ops) > 2:  # fixed point
            v = f"({v}) * {float(2 ** ops[2].imm)!r}"
        v = {"z": f"std::trunc({v})", "m": f"std::floor({v})", "p": f"std::ceil({v})",
             "a": f"nx::round_away({v})", "n": f"nx::round_even({v})"}[rnd]
        t = f"{s(d.bits) if signed else u(d.bits)}"
        return d.w(f"{u(d.bits)}(nx::fcvt_sat<{t}>({v}))")

    def i_fcvtzs(self, f, i, ops):
        return self.cvt_to_int(i, ops, "z", True)

    def i_fcvtzu(self, f, i, ops):
        return self.cvt_to_int(i, ops, "z", False)

    def i_fcvtms(self, f, i, ops):
        return self.cvt_to_int(i, ops, "m", True)

    def i_fcvtmu(self, f, i, ops):
        return self.cvt_to_int(i, ops, "m", False)

    def i_fcvtps(self, f, i, ops):
        return self.cvt_to_int(i, ops, "p", True)

    def i_fcvtpu(self, f, i, ops):
        return self.cvt_to_int(i, ops, "p", False)

    def i_fcvtas(self, f, i, ops):
        return self.cvt_to_int(i, ops, "a", True)

    def i_fcvtau(self, f, i, ops):
        return self.cvt_to_int(i, ops, "a", False)

    def i_fcvtns(self, f, i, ops):
        return self.cvt_to_int(i, ops, "n", True)

    def i_fcvtnu(self, f, i, ops):
        return self.cvt_to_int(i, ops, "n", False)

    def cvt_from_int(self, i, ops, signed):
        if self.is_vec(i, ops[0]):
            return self.v_un(i, ops, "a", fp_from_int=("s" if signed else "u"))
        d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
        ty = "float" if d.kind in ("s", "h") else "double"
        if n.gp:
            v = f"{s(n.bits) if signed else u(n.bits)}({n.r()})"
        else:  # scvtf s0, s1 (integer in simd register)
            v = f"c.v[{n.n}].{'s' if signed else 'u'}{n.bits}[0]"
        v = f"{ty}({v})"
        if len(ops) > 2:
            v = f"{v} / {ty}({float(2 ** ops[2].imm)!r})"
        return self.fw(d, v)

    def i_scvtf(self, f, i, ops):
        return self.cvt_from_int(i, ops, True)

    def i_ucvtf(self, f, i, ops):
        return self.cvt_from_int(i, ops, False)

    # ---- NEON ---------------------------------------------------------------------------------------------

    def vinfo(self, o):
        if not o.vas:
            raise Unhandled("vector without arrangement")
        return VAS[o.vas]

    def lane_t(self, bits, kind):
        return f"{kind}{bits}"

    def v_src(self, i, o, bits, kind, k):
        """Lane k of a vector source operand; by-element operands broadcast their lane."""
        r = self.regop(i, o)
        if o.vector_index != -1:
            return f"c.v[{r.n}].{kind}{bits}[{o.vector_index}]"
        return f"c.v[{r.n}].{kind}{bits}[{k}]"

    def v_loop(self, i, dst, lanes, bits, body_fn, keep_dst=False):
        d = self.regop(i, dst)
        init = f"c.v[{d.n}]" if keep_dst else "nx::vzero()"
        return (f"{{ nx::V128 r = {init}; for (int k = 0; k < {lanes}; k++) {{ {body_fn()} }} c.v[{d.n}] = r; }}")

    def v_bin(self, i, ops, expr, fp=False, signed=False, lanes=None, acc=None):
        """Lanewise binary op; `acc` means the destination is also an input (o)."""
        bits, n = self.vinfo(ops[0])
        if lanes == "bytes":
            n = n * bits // 8
            bits = 8
        kind = "f" if fp else ("s" if signed else "u")
        d = self.regop(i, ops[0])
        srcs = [f"A[{k}]" for k in range(len(ops) - 1)]
        pre = []
        for k, o in enumerate(ops[1:3]):
            r = self.regop(i, o)
            pre.append(f"nx::V128 S{k} = c.v[{r.n}];")
        a = "S0.{t}[k]".format(t=f"{kind}{bits}")
        if ops[2].vector_index != -1:
            b = f"S1.{kind}{bits}[{ops[2].vector_index}]"
        else:
            b = f"S1.{kind}{bits}[k]"
        o_ = f"O.{kind}{bits}[k]"
        e = subst(expr, a=a, b=b, o=o_)
        ty = {("f", 32): "float", ("f", 64): "double"}.get((kind, bits), f"{'int' if kind == 's' else 'uint'}{bits}_t")
        pre.append(f"nx::V128 O = c.v[{d.n}];")
        return (f"{{ {' '.join(pre)} nx::V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) r.{kind}{bits}[k] = {ty}({e}); "
                f"c.v[{d.n}] = r; }}")

    def v_un(self, i, ops, expr, fp=False, signed=False, lanes=None, fp_from_int=None):
        bits, n = self.vinfo(ops[0])
        if lanes == "bytes":
            n = n * bits // 8
            bits = 8
        kind = "f" if fp or fp_from_int else ("s" if signed else "u")
        skind = fp_from_int or kind
        d, m = self.regop(i, ops[0]), self.regop(i, ops[1])
        ty = {("f", 32): "float", ("f", 64): "double"}.get((kind, bits), f"{'int' if kind == 's' else 'uint'}{bits}_t")
        src = f"S.{skind}{bits}[k]" if ops[1].vector_index == -1 else f"S.{skind}{bits}[{ops[1].vector_index}]"
        e = subst(expr, a=src)
        return (f"{{ nx::V128 S = c.v[{m.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) "
                f"r.{kind}{bits}[k] = {ty}({e}); c.v[{d.n}] = r; }}")

    def v_mov(self, f, i, ops):
        d, n = ops[0], ops[1]
        dr = self.regop(i, d)
        if n.type == A.ARM64_OP_REG:
            nr = self.regop(i, n)
            if d.vector_index != -1 and nr.gp:  # mov v0.s[1], w8  (ins)
                bits = VAS[d.vas][0] if d.vas else 32
                return f"c.v[{dr.n}].u{bits}[{d.vector_index}] = uint{bits}_t({nr.r()});"
            if d.vector_index != -1 and n.vector_index != -1:  # mov v0.s[1], v1.s[0]
                bits = VAS[d.vas][0] if d.vas else 32
                return f"c.v[{dr.n}].u{bits}[{d.vector_index}] = c.v[{nr.n}].u{bits}[{n.vector_index}];"
            if dr.gp and n.vector_index != -1:  # mov w0, v0.s[1]  (umov)
                bits = VAS[n.vas][0] if n.vas else dr.bits
                return dr.w(f"c.v[{nr.n}].u{bits}[{n.vector_index}]")
            if not dr.gp and n.vector_index != -1:  # mov s0, v1.s[1]  (dup scalar)
                bits = VAS[n.vas][0] if n.vas else dr.bits
                return f"{{ nx::V128 t_ = nx::vzero(); t_.u{bits}[0] = c.v[{nr.n}].u{bits}[{n.vector_index}]; c.v[{dr.n}] = t_; }}"
            if d.vas and n.vas:  # mov v0.16b, v1.16b
                bits, lanes = VAS[d.vas]
                if bits * lanes == 64:
                    return f"{{ nx::V128 t_ = nx::vzero(); t_.u64[0] = c.v[{nr.n}].u64[0]; c.v[{dr.n}] = t_; }}"
                return f"c.v[{dr.n}] = c.v[{nr.n}];"
        raise Unhandled("mov vector form")

    def i_umov(self, f, i, ops):
        return self.v_mov(f, i, ops)

    def i_smov(self, f, i, ops):
        d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
        bits = VAS[ops[1].vas][0]
        return d.w(f"{u(d.bits)}({s(d.bits)}(c.v[{n.n}].s{bits}[{ops[1].vector_index}]))")

    def i_ins(self, f, i, ops):
        return self.v_mov(f, i, ops)

    def i_dup(self, f, i, ops):
        bits, lanes = self.vinfo(ops[0])
        d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
        if n.gp:
            v = f"uint{bits}_t({n.r()})"
        else:
            v = f"c.v[{n.n}].u{bits}[{ops[1].vector_index}]"
        return f"{{ uint{bits}_t e_ = {v}; nx::V128 r = nx::vzero(); for (int k = 0; k < {lanes}; k++) r.u{bits}[k] = e_; c.v[{d.n}] = r; }}"

    def i_movi(self, f, i, ops):
        d = self.regop(i, ops[0])
        imm = ops[1].imm
        if ops[1].shift.type == A.ARM64_SFT_LSL:
            imm <<= ops[1].shift.value
        elif ops[1].shift.type == A.ARM64_SFT_MSL:
            imm = (imm << ops[1].shift.value) | ((1 << ops[1].shift.value) - 1)
        if not ops[0].vas:  # movi d0, #imm  (64-bit scalar, byte mask)
            return f"{{ nx::V128 r = nx::vzero(); r.u64[0] = {imm & 0xFFFFFFFFFFFFFFFF:#x}ull; c.v[{d.n}] = r; }}"
        bits, lanes = self.vinfo(ops[0])
        return (f"{{ nx::V128 r = nx::vzero(); for (int k = 0; k < {lanes}; k++) r.u{bits}[k] = "
                f"uint{bits}_t({imm & ((1 << bits) - 1):#x}ull); c.v[{d.n}] = r; }}")

    def i_mvni(self, f, i, ops):
        d = self.regop(i, ops[0])
        bits, lanes = self.vinfo(ops[0])
        imm = ops[1].imm
        if ops[1].shift.type == A.ARM64_SFT_LSL:
            imm <<= ops[1].shift.value
        elif ops[1].shift.type == A.ARM64_SFT_MSL:
            imm = (imm << ops[1].shift.value) | ((1 << ops[1].shift.value) - 1)
        return (f"{{ nx::V128 r = nx::vzero(); for (int k = 0; k < {lanes}; k++) r.u{bits}[k] = "
                f"uint{bits}_t(~{imm:#x}ull); c.v[{d.n}] = r; }}")

    def v_imm_logic(self, i, ops, op):
        d = self.regop(i, ops[0])
        bits, lanes = self.vinfo(ops[0])
        imm = ops[1].imm << (ops[1].shift.value if ops[1].shift.type else 0)
        e = f"r.u{bits}[k] | uint{bits}_t({imm:#x}ull)" if op == "|" else f"r.u{bits}[k] & uint{bits}_t(~{imm:#x}ull)"
        clear = "" if bits * lanes == 128 else " r.u64[1] = 0;"
        return f"{{ nx::V128 r = c.v[{d.n}]; for (int k = 0; k < {lanes}; k++) r.u{bits}[k] = {e};{clear} c.v[{d.n}] = r; }}"

    def i_fmla(self, f, i, ops):
        if not ops[0].vas:  # scalar by element: fmla s0, s1, v2.s[1]
            d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
            return self.fw(d, f"std::fma({self.fr(n)}, {self.velem(i, ops[2], 'f')}, {self.fr(d)})")
        return self.v_bin(i, ops, "std::fma(a, b, o)", fp=True)

    def i_fmls(self, f, i, ops):
        if not ops[0].vas:
            d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
            return self.fw(d, f"std::fma(-({self.fr(n)}), {self.velem(i, ops[2], 'f')}, {self.fr(d)})")
        return self.v_bin(i, ops, "std::fma(-a, b, o)", fp=True)

    def i_mla(self, f, i, ops):
        return self.v_bin(i, ops, "o + a * b")

    def i_mls(self, f, i, ops):
        return self.v_bin(i, ops, "o - a * b")

    def i_bsl(self, f, i, ops):
        return self.v_bin(i, ops, "(o & a) | (~o & b)", lanes="bytes")

    def i_bit(self, f, i, ops):
        return self.v_bin(i, ops, "(o & ~b) | (a & b)", lanes="bytes")

    def i_bif(self, f, i, ops):
        return self.v_bin(i, ops, "(o & b) | (a & ~b)", lanes="bytes")

    def i_smax(self, f, i, ops):
        return self.v_bin(i, ops, "a > b ? a : b", signed=True)

    def i_smin(self, f, i, ops):
        return self.v_bin(i, ops, "a < b ? a : b", signed=True)

    def i_umax(self, f, i, ops):
        return self.v_bin(i, ops, "a > b ? a : b")

    def i_umin(self, f, i, ops):
        return self.v_bin(i, ops, "a < b ? a : b")

    def i_urhadd(self, f, i, ops):
        return self.v_bin(i, ops, "(uint64_t(a) + uint64_t(b) + 1) >> 1")

    def i_srhadd(self, f, i, ops):
        return self.v_bin(i, ops, "(int64_t(a) + int64_t(b) + 1) >> 1", signed=True)

    def i_uhadd(self, f, i, ops):
        return self.v_bin(i, ops, "(uint64_t(a) + uint64_t(b)) >> 1")

    def i_shadd(self, f, i, ops):
        return self.v_bin(i, ops, "(int64_t(a) + int64_t(b)) >> 1", signed=True)

    def i_sabd(self, f, i, ops):
        return self.v_bin(i, ops, "a > b ? a - b : b - a", signed=True)

    def i_uabd(self, f, i, ops):
        return self.v_bin(i, ops, "a > b ? a - b : b - a")

    def i_sqdmulh(self, f, i, ops):
        bits = self.vinfo(ops[0])[0]
        return self.v_bin(i, ops, f"nx::sat<int{bits}_t>((int64_t(a) * int64_t(b) * 2) >> {bits})", signed=True)

    def i_sshl(self, f, i, ops):
        bits = self.vinfo(ops[0])[0]
        return self.v_bin(i, ops, f"int8_t(S1.u{bits}[k]) >= 0 ? (int8_t(S1.u{bits}[k]) >= {bits} ? 0 : int64_t(a) << int8_t(S1.u{bits}[k])) : (int8_t(S1.u{bits}[k]) <= -{bits} ? (a < 0 ? -1 : 0) : int64_t(a) >> -int8_t(S1.u{bits}[k]))", signed=True)

    def i_ushl(self, f, i, ops):
        bits = self.vinfo(ops[0])[0]
        return self.v_bin(i, ops, f"int8_t(S1.u{bits}[k]) >= 0 ? (int8_t(S1.u{bits}[k]) >= {bits} ? 0 : uint64_t(a) << int8_t(S1.u{bits}[k])) : (int8_t(S1.u{bits}[k]) <= -{bits} ? 0 : uint64_t(a) >> -int8_t(S1.u{bits}[k]))")

    def cmp_mask(self, i, ops, op, kind):
        bits, n = self.vinfo(ops[0])
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        if ops[2].type in (A.ARM64_OP_IMM, A.ARM64_OP_FP):
            b = "0"
        else:
            b = f"c.v[{self.regop(i, ops[2]).n}].{kind}{bits}[k]"
        return (f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 T = c.v[{self.regop(i, ops[2]).n if ops[2].type == A.ARM64_OP_REG else a.n}]; nx::V128 r = nx::vzero(); "
                f"for (int k = 0; k < {n}; k++) r.u{bits}[k] = (S.{kind}{bits}[k] {op} {b.replace(f'c.v[{self.regop(i, ops[2]).n if ops[2].type == A.ARM64_OP_REG else a.n}]', 'T')}) ? uint{bits}_t(~0ull) : 0; c.v[{d.n}] = r; }}")

    def i_cmeq(self, f, i, ops):
        if not ops[0].vas:  # cmeq d0, d1, d2 / cmeq d0, d1, #0
            d, n = self.regop(i, ops[0]), self.regop(i, ops[1])
            b = "0" if ops[2].type != A.ARM64_OP_REG else f"c.v[{self.regop(i, ops[2]).n}].u64[0]"
            return (f"{{ uint64_t r_ = c.v[{n.n}].u64[0] == {b} ? ~0ull : 0; nx::V128 t_ = nx::vzero(); "
                    f"t_.u64[0] = r_; c.v[{d.n}] = t_; }}")
        return self.cmp_mask(i, ops, "==", "u")

    def i_cmgt(self, f, i, ops):
        return self.cmp_mask(i, ops, ">", "s")

    def i_cmge(self, f, i, ops):
        return self.cmp_mask(i, ops, ">=", "s")

    def i_cmhi(self, f, i, ops):
        return self.cmp_mask(i, ops, ">", "u")

    def i_cmhs(self, f, i, ops):
        return self.cmp_mask(i, ops, ">=", "u")

    def i_cmlt(self, f, i, ops):
        return self.cmp_mask(i, ops, "<", "s")

    def i_cmle(self, f, i, ops):
        return self.cmp_mask(i, ops, "<=", "s")

    def i_cmtst(self, f, i, ops):
        return self.v_bin(i, ops, "(a & b) ? ~0ull : 0")

    def i_fcmeq(self, f, i, ops):
        return self.cmp_mask(i, ops, "==", "f")

    def i_fcmgt(self, f, i, ops):
        return self.cmp_mask(i, ops, ">", "f")

    def i_fcmge(self, f, i, ops):
        return self.cmp_mask(i, ops, ">=", "f")

    def i_fcmlt(self, f, i, ops):
        return self.cmp_mask(i, ops, "<", "f")

    def i_fcmle(self, f, i, ops):
        return self.cmp_mask(i, ops, "<=", "f")

    # shifts by immediate
    def v_shift_imm(self, i, ops, expr, signed=False, acc=False):
        bits, n = self.vinfo(ops[0])
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        sh = ops[2].imm
        kind = "s" if signed else "u"
        e = expr.format(a=f"int64_t(S.{kind}{bits}[k])" if signed else f"uint64_t(S.{kind}{bits}[k])", n=sh,
                        bits=bits, o=f"r.{kind}{bits}[k]")
        init = f"c.v[{d.n}]" if acc else "nx::vzero()"
        return (f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 r = {init}; for (int k = 0; k < {n}; k++) "
                f"r.{kind}{bits}[k] = {'int' if signed else 'uint'}{bits}_t({e}); "
                + ("" if bits * n == 128 else "r.u64[1] = 0; ") + f"c.v[{d.n}] = r; }}")

    def i_shl(self, f, i, ops):
        return self.v_shift_imm(i, ops, "{a} << {n}")

    def i_ushr(self, f, i, ops):
        return self.v_shift_imm(i, ops, "({n} >= {bits}) ? 0 : ({a} >> {n})")

    def i_sshr(self, f, i, ops):
        return self.v_shift_imm(i, ops, "{a} >> ({n} >= {bits} ? {bits} - 1 : {n})", signed=True)

    def i_srshr(self, f, i, ops):
        return self.v_shift_imm(i, ops, "({a} + (int64_t(1) << ({n} - 1))) >> {n}", signed=True)

    def i_urshr(self, f, i, ops):
        return self.v_shift_imm(i, ops, "({a} + (uint64_t(1) << ({n} - 1))) >> {n}")

    def i_ssra(self, f, i, ops):
        return self.v_shift_imm(i, ops, "{o} + ({a} >> ({n} >= {bits} ? {bits} - 1 : {n}))", signed=True, acc=True)

    def i_usra(self, f, i, ops):
        return self.v_shift_imm(i, ops, "{o} + (({n} >= {bits}) ? 0 : ({a} >> {n}))", acc=True)

    def i_srsra(self, f, i, ops):
        return self.v_shift_imm(i, ops, "{o} + (({a} + (int64_t(1) << ({n} - 1))) >> {n})", signed=True, acc=True)

    def i_ursra(self, f, i, ops):
        return self.v_shift_imm(i, ops, "{o} + (({a} + (uint64_t(1) << ({n} - 1))) >> {n})", acc=True)

    # widening / narrowing
    def widen(self, i, ops, expr, signed, second_wide=False, acc=False):
        """Destination lanes are 2x the source lanes. `2` variants read the upper half of narrow sources."""
        dbits, n = self.vinfo(ops[0])
        hi = i.mnemonic.endswith("2")
        sbits = dbits // 2
        kind = "s" if signed else "u"
        d = self.regop(i, ops[0])
        srcs = []
        pre = []
        for k, o in enumerate(ops[1:3] if len(ops) > 2 and ops[2].type == A.ARM64_OP_REG else ops[1:2]):
            r = self.regop(i, o)
            pre.append(f"nx::V128 S{k} = c.v[{r.n}];")
            wide = second_wide and k == 0
            if o.vector_index != -1:
                srcs.append(f"int64_t(S{k}.{kind}{sbits}[{o.vector_index}])" if signed else f"uint64_t(S{k}.{kind}{sbits}[{o.vector_index}])")
            elif wide:
                srcs.append(f"{'int64_t' if signed else 'uint64_t'}(S{k}.{kind}{dbits}[k])")
            else:
                idx = f"k + {n}" if hi else "k"
                srcs.append(f"{'int64_t' if signed else 'uint64_t'}(S{k}.{kind}{sbits}[{idx}])")
        imm = ops[2].imm if len(ops) > 2 and ops[2].type == A.ARM64_OP_IMM else 0
        e = expr.format(a=srcs[0], b=srcs[1] if len(srcs) > 1 else "0", n=imm, o=f"O.{kind}{dbits}[k]")
        return (f"{{ {' '.join(pre)} nx::V128 O = c.v[{d.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) "
                f"r.{kind}{dbits}[k] = {'int' if signed else 'uint'}{dbits}_t({e}); c.v[{d.n}] = r; }}")

    def i_ushll(self, f, i, ops):
        return self.widen(i, ops, "{a} << {n}", False)

    i_ushll2 = i_ushll

    def i_sshll(self, f, i, ops):
        return self.widen(i, ops, "{a} << {n}", True)

    i_sshll2 = i_sshll

    def i_uxtl(self, f, i, ops):
        return self.widen(i, ops, "{a}", False)

    i_uxtl2 = i_uxtl

    def i_sxtl(self, f, i, ops):
        return self.widen(i, ops, "{a}", True)

    i_sxtl2 = i_sxtl

    def i_shll(self, f, i, ops):
        bits = self.vinfo(ops[0])[0] // 2
        return self.widen(i, ops, "{a} << " + str(bits), False)

    i_shll2 = i_shll

    def i_uaddw(self, f, i, ops):
        return self.widen(i, ops, "{a} + {b}", False, second_wide=True)

    i_uaddw2 = i_uaddw

    def i_saddw(self, f, i, ops):
        return self.widen(i, ops, "{a} + {b}", True, second_wide=True)

    i_saddw2 = i_saddw

    def i_usubw(self, f, i, ops):
        return self.widen(i, ops, "{a} - {b}", False, second_wide=True)

    i_usubw2 = i_usubw

    def i_ssubw(self, f, i, ops):
        return self.widen(i, ops, "{a} - {b}", True, second_wide=True)

    i_ssubw2 = i_ssubw

    def i_uaddl(self, f, i, ops):
        return self.widen(i, ops, "{a} + {b}", False)

    i_uaddl2 = i_uaddl

    def i_saddl(self, f, i, ops):
        return self.widen(i, ops, "{a} + {b}", True)

    i_saddl2 = i_saddl

    def i_usubl(self, f, i, ops):
        return self.widen(i, ops, "{a} - {b}", False)

    i_usubl2 = i_usubl

    def i_ssubl(self, f, i, ops):
        return self.widen(i, ops, "{a} - {b}", True)

    i_ssubl2 = i_ssubl

    def v_long(self, i, ops, expr, signed, high):
        return self.widen(i, ops, "{a} * {b}", signed)

    i_umull2 = lambda self, f, i, ops: self.widen(i, ops, "{a} * {b}", False)
    i_smull2 = lambda self, f, i, ops: self.widen(i, ops, "{a} * {b}", True)

    def i_umlal(self, f, i, ops):
        return self.widen(i, ops, "{o} + {a} * {b}", False)

    i_umlal2 = i_umlal

    def i_smlal(self, f, i, ops):
        return self.widen(i, ops, "{o} + {a} * {b}", True)

    i_smlal2 = i_smlal

    def pairwise_long(self, i, ops, signed):
        dbits, n = self.vinfo(ops[0])
        sbits = dbits // 2
        kind = "s" if signed else "u"
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        return (f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) "
                f"r.{kind}{dbits}[k] = {'int' if signed else 'uint'}{dbits}_t({'int64_t' if signed else 'uint64_t'}(S.{kind}{sbits}[2 * k]) + S.{kind}{sbits}[2 * k + 1]); c.v[{d.n}] = r; }}")

    def i_saddlp(self, f, i, ops):
        return self.pairwise_long(i, ops, True)

    def i_uaddlp(self, f, i, ops):
        return self.pairwise_long(i, ops, False)

    def narrow(self, i, ops, expr, src_kind="u", dst_kind="u"):
        """Destination lanes are half the source lanes; `2` variants write the upper half and keep the lower."""
        dbits, n = self.vinfo(ops[0])
        hi = i.mnemonic.endswith("2")
        sbits = dbits * 2
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        cnt = n // 2 if hi else n
        base = n // 2 if hi else 0
        imm = ops[2].imm if len(ops) > 2 and ops[2].type == A.ARM64_OP_IMM else 0
        sty = "int64_t" if src_kind == "s" else "uint64_t"
        e = expr.format(a=f"{sty}(S.{src_kind}{sbits}[k])", n=imm, dbits=dbits)
        init = f"c.v[{d.n}]" if hi else "nx::vzero()"
        return (f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 r = {init}; for (int k = 0; k < {cnt}; k++) "
                f"r.{dst_kind}{dbits}[{base} + k] = {'int' if dst_kind == 's' else 'uint'}{dbits}_t({e}); c.v[{d.n}] = r; }}")

    def i_xtn(self, f, i, ops):
        return self.narrow(i, ops, "{a}")

    i_xtn2 = i_xtn

    def i_shrn(self, f, i, ops):
        return self.narrow(i, ops, "{a} >> {n}")

    i_shrn2 = i_shrn

    def i_sqshrn(self, f, i, ops):
        return self.narrow(i, ops, "nx::sat<int{dbits}_t>({a} >> {n})", src_kind="s", dst_kind="s")

    i_sqshrn2 = i_sqshrn

    def i_sqrshrn(self, f, i, ops):
        return self.narrow(i, ops, "nx::sat<int{dbits}_t>(({a} + (int64_t(1) << ({n} - 1))) >> {n})",
                           src_kind="s", dst_kind="s")

    i_sqrshrn2 = i_sqrshrn

    def i_sqadd(self, f, i, ops):
        bits = self.vinfo(ops[0])[0]
        return self.v_bin(i, ops, f"nx::sat<int{bits}_t>(int64_t(a) + int64_t(b))", signed=True)

    def i_sqsub(self, f, i, ops):
        bits = self.vinfo(ops[0])[0]
        return self.v_bin(i, ops, f"nx::sat<int{bits}_t>(int64_t(a) - int64_t(b))", signed=True)

    def i_uqadd(self, f, i, ops):
        bits = self.vinfo(ops[0])[0]
        return self.v_bin(i, ops, f"(uint64_t(a) + uint64_t(b)) > uint{bits}_t(~0ull) ? uint{bits}_t(~0ull) : a + b")

    def i_uqsub(self, f, i, ops):
        return self.v_bin(i, ops, "a > b ? a - b : 0")

    def i_uaddlv(self, f, i, ops):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        bits, n = self.vinfo(ops[1])
        return (f"{{ uint64_t s_ = 0; for (int k = 0; k < {n}; k++) s_ += c.v[{a.n}].u{bits}[k]; "
                f"nx::V128 r = nx::vzero(); r.u{bits * 2}[0] = uint{bits * 2}_t(s_); c.v[{d.n}] = r; }}")

    def i_smaxv(self, f, i, ops):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        bits, n = self.vinfo(ops[1])
        return (f"{{ int{bits}_t s_ = c.v[{a.n}].s{bits}[0]; for (int k = 1; k < {n}; k++) s_ = c.v[{a.n}].s{bits}[k] > s_ ? c.v[{a.n}].s{bits}[k] : s_; "
                f"nx::V128 r = nx::vzero(); r.s{bits}[0] = s_; c.v[{d.n}] = r; }}")

    def i_frsqrts(self, f, i, ops):
        return self.fbin(i, ops, "nx::frsqrts(a, b)")

    def i_frecps(self, f, i, ops):
        return self.fbin(i, ops, "2.0f - a * b")

    def i_sqxtun(self, f, i, ops):
        return self.narrow(i, ops, "nx::sat<uint{dbits}_t>({a})", src_kind="s")

    i_sqxtun2 = i_sqxtun

    def i_sqxtn(self, f, i, ops):
        return self.narrow(i, ops, "nx::sat<int{dbits}_t>({a})", src_kind="s", dst_kind="s")

    i_sqxtn2 = i_sqxtn

    def i_uqxtn(self, f, i, ops):
        return self.narrow(i, ops, "{a} > uint{dbits}_t(~0ull) ? uint{dbits}_t(~0ull) : {a}")

    i_uqxtn2 = i_uqxtn

    def i_sqshrun(self, f, i, ops):
        return self.narrow(i, ops, "nx::sat<uint{dbits}_t>({a} >> {n})", src_kind="s")

    i_sqshrun2 = i_sqshrun

    def i_sqrshrun(self, f, i, ops):
        return self.narrow(i, ops, "nx::sat<uint{dbits}_t>(({a} + (int64_t(1) << ({n} - 1))) >> {n})", src_kind="s")

    i_sqrshrun2 = i_sqrshrun

    def i_fcvtn(self, f, i, ops):
        dbits, n = self.vinfo(ops[0])
        hi = i.mnemonic.endswith("2")
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        cnt = n // 2 if hi else n
        base = n // 2 if hi else 0
        init = f"c.v[{d.n}]" if hi else "nx::vzero()"
        if dbits == 32:
            body = f"r.f32[{base} + k] = float(S.f64[k]);"
        else:
            body = f"r.u16[{base} + k] = nx::f32_to_f16(S.f32[k]);"
        return f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 r = {init}; for (int k = 0; k < {cnt}; k++) {body} c.v[{d.n}] = r; }}"

    i_fcvtn2 = i_fcvtn

    def i_fcvtl(self, f, i, ops):
        dbits, n = self.vinfo(ops[0])
        hi = i.mnemonic.endswith("2")
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        off = f"k + {n}" if hi else "k"
        body = f"r.f64[k] = double(S.f32[{off}]);" if dbits == 64 else f"r.f32[k] = nx::f16_to_f32(S.u16[{off}]);"
        return f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) {body} c.v[{d.n}] = r; }}"

    i_fcvtl2 = i_fcvtl

    def v_cvt_to_int(self, i, ops, rnd, signed):
        bits, n = self.vinfo(ops[0])
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        v = f"S.f{bits}[k]"
        if len(ops) > 2:
            v = f"({v}) * {float(2 ** ops[2].imm)!r}"
        v = {"z": f"std::trunc({v})", "m": f"std::floor({v})", "p": f"std::ceil({v})",
             "a": f"nx::round_away({v})", "n": f"nx::round_even({v})"}[rnd]
        t = f"{'int' if signed else 'uint'}{bits}_t"
        return (f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) "
                f"r.{'s' if signed else 'u'}{bits}[k] = nx::fcvt_sat<{t}>({v}); c.v[{d.n}] = r; }}")

    # permutes
    def i_ext(self, f, i, ops):
        bits, n = self.vinfo(ops[0])
        nbytes = n * bits // 8
        d, a, b = (self.regop(i, o) for o in ops[:3])
        imm = ops[3].imm
        return (f"{{ uint8_t t_[32]; std::memcpy(t_, &c.v[{a.n}], {nbytes}); std::memcpy(t_ + {nbytes}, &c.v[{b.n}], {nbytes}); "
                f"nx::V128 r = nx::vzero(); std::memcpy(&r, t_ + {imm}, {nbytes}); c.v[{d.n}] = r; }}")

    def permute(self, i, ops, body):
        bits, n = self.vinfo(ops[0])
        d, a, b = (self.regop(i, o) for o in ops[:3])
        return (f"{{ nx::V128 A_ = c.v[{a.n}], B_ = c.v[{b.n}]; nx::V128 r = nx::vzero(); "
                f"for (int k = 0; k < {n // 2}; k++) {{ {body.format(t=f'u{bits}', h=n // 2)} }} c.v[{d.n}] = r; }}")

    def i_zip1(self, f, i, ops):
        return self.permute(i, ops, "r.{t}[2 * k] = A_.{t}[k]; r.{t}[2 * k + 1] = B_.{t}[k];")

    def i_zip2(self, f, i, ops):
        return self.permute(i, ops, "r.{t}[2 * k] = A_.{t}[k + {h}]; r.{t}[2 * k + 1] = B_.{t}[k + {h}];")

    def i_uzp1(self, f, i, ops):
        return self.permute(i, ops, "r.{t}[k] = A_.{t}[2 * k]; r.{t}[k + {h}] = B_.{t}[2 * k];")

    def i_uzp2(self, f, i, ops):
        return self.permute(i, ops, "r.{t}[k] = A_.{t}[2 * k + 1]; r.{t}[k + {h}] = B_.{t}[2 * k + 1];")

    def i_trn1(self, f, i, ops):
        return self.permute(i, ops, "r.{t}[2 * k] = A_.{t}[2 * k]; r.{t}[2 * k + 1] = B_.{t}[2 * k];")

    def i_trn2(self, f, i, ops):
        return self.permute(i, ops, "r.{t}[2 * k] = A_.{t}[2 * k + 1]; r.{t}[2 * k + 1] = B_.{t}[2 * k + 1];")

    def v_rev(self, i, ops, container):
        bits, n = self.vinfo(ops[0])
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        per = container // bits
        return (f"{{ nx::V128 S = c.v[{a.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n}; k++) "
                f"r.u{bits}[k] = S.u{bits}[(k / {per}) * {per} + ({per} - 1 - k % {per})]; c.v[{d.n}] = r; }}")

    def i_tbl(self, f, i, ops):
        bits, n = self.vinfo(ops[0])
        d = self.regop(i, ops[0])
        tables = [self.regop(i, o) for o in ops[1:-1]]
        idx = self.regop(i, ops[-1])
        cp = " ".join(f"std::memcpy(t_ + {16 * k}, &c.v[{r.n}], 16);" for k, r in enumerate(tables))
        return (f"{{ uint8_t t_[64]; {cp} nx::V128 I_ = c.v[{idx.n}]; nx::V128 r = nx::vzero(); "
                f"for (int k = 0; k < {n}; k++) r.u8[k] = I_.u8[k] < {16 * len(tables)} ? t_[I_.u8[k]] : 0; c.v[{d.n}] = r; }}")

    # reductions
    def i_addv(self, f, i, ops):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        bits, n = self.vinfo(ops[1])
        return (f"{{ uint64_t s_ = 0; for (int k = 0; k < {n}; k++) s_ += c.v[{a.n}].u{bits}[k]; "
                f"nx::V128 r = nx::vzero(); r.u{bits}[0] = uint{bits}_t(s_); c.v[{d.n}] = r; }}")

    def i_umaxv(self, f, i, ops):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        bits, n = self.vinfo(ops[1])
        return (f"{{ uint{bits}_t s_ = 0; for (int k = 0; k < {n}; k++) s_ = c.v[{a.n}].u{bits}[k] > s_ ? c.v[{a.n}].u{bits}[k] : s_; "
                f"nx::V128 r = nx::vzero(); r.u{bits}[0] = s_; c.v[{d.n}] = r; }}")

    def i_fmaxnmv(self, f, i, ops):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        return (f"{{ nx::V128 S = c.v[{a.n}]; float s_ = nx::fmaxnm(nx::fmaxnm(S.f32[0], S.f32[1]), nx::fmaxnm(S.f32[2], S.f32[3])); "
                f"nx::V128 r = nx::vzero(); r.f32[0] = s_; c.v[{d.n}] = r; }}")

    def i_fminnmv(self, f, i, ops):
        d, a = self.regop(i, ops[0]), self.regop(i, ops[1])
        return (f"{{ nx::V128 S = c.v[{a.n}]; float s_ = nx::fminnm(nx::fminnm(S.f32[0], S.f32[1]), nx::fminnm(S.f32[2], S.f32[3])); "
                f"nx::V128 r = nx::vzero(); r.f32[0] = s_; c.v[{d.n}] = r; }}")

    def i_faddp(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not ops[0].vas:  # faddp s0, v1.2s
            a = self.regop(i, ops[1])
            bits = VAS[ops[1].vas][0]
            return self.fw(d, f"c.v[{a.n}].f{bits}[0] + c.v[{a.n}].f{bits}[1]")
        bits, n = self.vinfo(ops[0])
        a, b = self.regop(i, ops[1]), self.regop(i, ops[2])
        return (f"{{ nx::V128 A_ = c.v[{a.n}], B_ = c.v[{b.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n // 2}; k++) "
                f"{{ r.f{bits}[k] = A_.f{bits}[2 * k] + A_.f{bits}[2 * k + 1]; r.f{bits}[k + {n // 2}] = B_.f{bits}[2 * k] + B_.f{bits}[2 * k + 1]; }} c.v[{d.n}] = r; }}")

    def i_addp(self, f, i, ops):
        d = self.regop(i, ops[0])
        if not ops[0].vas:  # addp d0, v1.2d
            a = self.regop(i, ops[1])
            return f"{{ nx::V128 r = nx::vzero(); r.u64[0] = c.v[{a.n}].u64[0] + c.v[{a.n}].u64[1]; c.v[{d.n}] = r; }}"
        bits, n = self.vinfo(ops[0])
        a, b = self.regop(i, ops[1]), self.regop(i, ops[2])
        return (f"{{ nx::V128 A_ = c.v[{a.n}], B_ = c.v[{b.n}]; nx::V128 r = nx::vzero(); for (int k = 0; k < {n // 2}; k++) "
                f"{{ r.u{bits}[k] = A_.u{bits}[2 * k] + A_.u{bits}[2 * k + 1]; r.u{bits}[k + {n // 2}] = B_.u{bits}[2 * k] + B_.u{bits}[2 * k + 1]; }} c.v[{d.n}] = r; }}")

    def i_cnt(self, f, i, ops):
        return self.v_un(i, ops, "std::popcount(a)")

    def i_not(self, f, i, ops):
        return self.v_un(i, ops, "~a", lanes="bytes")

    def i_abs(self, f, i, ops):
        return self.v_un(i, ops, "a < 0 ? -a : a", signed=True)

    def i_frecpe(self, f, i, ops):
        return self.fun(i, ops, "1.0f / a")

    def i_frsqrte(self, f, i, ops):
        return self.fun(i, ops, "1.0f / std::sqrt(a)")

    # structured loads / stores
    def vlist(self, i, ops):
        regs, mem, after = self.mem_ops(ops)
        return [self.regop(i, o) for o in regs], regs, mem, after

    def vldst(self, i, ops, nstruct, store, replicate=False):
        rs, regs, mem, after = self.vlist(i, ops)
        o0 = regs[0]
        bits, lanes = VAS[o0.vas]
        a, wb = self.address(i, mem, after)
        if i.writeback and after is None:
            # Immediate post-index: capstone drops the operand; it always equals the bytes transferred.
            per_reg = bits // 8 if (o0.vector_index != -1 or replicate) else bits * lanes // 8
            wb = f"c.x[{Reg(i.reg_name(mem.mem.base)).n}] += {per_reg * len(rs)};"
            a = f"c.x[{Reg(i.reg_name(mem.mem.base)).n}]"
        parts = [f"uint64_t a = {a};"]
        esz = bits // 8
        lane_m = re.search(r"\}\[(\d+)\]", i.op_str)  # capstone omits the lane index on multi-register lists
        if o0.vector_index != -1 or lane_m:  # single lane
            lane = o0.vector_index if o0.vector_index != -1 else int(lane_m.group(1))
            for k, r in enumerate(rs):
                if store:
                    parts.append(f"nx::st<uint{bits}_t>(a + {k * esz}, c.v[{r.n}].u{bits}[{lane}]);")
                else:
                    parts.append(f"c.v[{r.n}].u{bits}[{lane}] = nx::ld<uint{bits}_t>(a + {k * esz});")
        elif replicate:
            for k, r in enumerate(rs):
                parts.append(f"{{ uint{bits}_t e_ = nx::ld<uint{bits}_t>(a + {k * esz}); nx::V128 r_ = nx::vzero(); "
                             f"for (int j = 0; j < {lanes}; j++) r_.u{bits}[j] = e_; c.v[{r.n}] = r_; }}")
        elif nstruct == 1:
            size = bits * lanes // 8
            for k, r in enumerate(rs):
                if store:
                    parts.append(f"std::memcpy(reinterpret_cast<void*>(a + {k * size}), &c.v[{r.n}], {size});")
                else:
                    parts.append(f"{{ nx::V128 r_ = nx::vzero(); std::memcpy(&r_, reinterpret_cast<const void*>(a + {k * size}), {size}); c.v[{r.n}] = r_; }}")
        else:
            # Interleaved: element j of structure s is at a + (j * nstruct + s) * esz.
            if store:
                for s_, r in enumerate(rs):
                    parts.append(f"for (int j = 0; j < {lanes}; j++) nx::st<uint{bits}_t>(a + (j * {nstruct} + {s_}) * {esz}, c.v[{r.n}].u{bits}[j]);")
            else:
                tmp = []
                for s_, r in enumerate(rs):
                    parts.append(f"nx::V128 t{s_} = nx::vzero(); for (int j = 0; j < {lanes}; j++) t{s_}.u{bits}[j] = nx::ld<uint{bits}_t>(a + (j * {nstruct} + {s_}) * {esz});")
                    tmp.append(f"c.v[{r.n}] = t{s_};")
                parts += tmp
        parts.append(wb)
        return "{ " + " ".join(p for p in parts if p) + " }"

    def i_ld1(self, f, i, ops):
        return self.vldst(i, ops, 1, False)

    def i_st1(self, f, i, ops):
        return self.vldst(i, ops, 1, True)

    def i_ld1r(self, f, i, ops):
        return self.vldst(i, ops, 1, False, replicate=True)

    def i_ld2(self, f, i, ops):
        return self.vldst(i, ops, 2, False)

    def i_st2(self, f, i, ops):
        return self.vldst(i, ops, 2, True)

    def i_ld3(self, f, i, ops):
        return self.vldst(i, ops, 3, False)

    def i_st3(self, f, i, ops):
        return self.vldst(i, ops, 3, True)

    def i_ld4(self, f, i, ops):
        return self.vldst(i, ops, 4, False)

    def i_st4(self, f, i, ops):
        return self.vldst(i, ops, 4, True)
