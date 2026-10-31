// Vector loads and stores, and instructions that move data between elements: permute, select,
// shifts by bits and octets, merge, splat, pack and unpack.
// Reference: Power ISA v3.1, Book I, chapter 6, "Vector Facility".

#include "cpu/ppu/vector_ops.h"

namespace ppu {

namespace {

using namespace vmx;

// ---- Loads and stores ----
// Vector accesses ignore the low bits of the address that would make them unaligned.

u64 vector_address(const Thread& t, Instruction op) {
    return t.gpr_or_zero(op.ra()) + t.state.gpr[op.rb()];
}

Vector load_quadword(Thread& t, u64 address) {
    Vector v;
    t.memory.read_bytes(address & ~u64{15}, v.bytes);
    return v;
}

void store_quadword(Thread& t, u64 address, const Vector& v) {
    t.memory.write_bytes(address & ~u64{15}, v.bytes);
}

// lvx and lvxl (the "l" form is a cache hint: "least recently used").
void lvx(Thread& t, Instruction op) {
    set_vd(t, op, load_quadword(t, vector_address(t, op)));
}

void stvx(Thread& t, Instruction op) {
    store_quadword(t, vector_address(t, op), t.state.vr[op.vs()]);
}

// lvebx, lvehx and lvewx load one element into its natural position in the register. The
// manual leaves the other elements undefined. UNVERIFIED: we load the whole aligned quadword,
// which is a common implementation and keeps the loaded element in place.
void load_element(Thread& t, Instruction op) {
    set_vd(t, op, load_quadword(t, vector_address(t, op)));
}

void lvebx(Thread& t, Instruction op) {
    load_element(t, op);
}
void lvehx(Thread& t, Instruction op) {
    load_element(t, op);
}
void lvewx(Thread& t, Instruction op) {
    load_element(t, op);
}

// stvebx, stvehx and stvewx store the element at the address's position in the quadword.
void store_element(Thread& t, Instruction op, u64 alignment) {
    const u64 address = vector_address(t, op) & ~(alignment - 1);
    const u64 offset = address & 15;
    const Vector& v = t.state.vr[op.vs()];
    t.memory.write_bytes(address, std::span<const u8>(v.bytes.data() + offset, alignment));
}

void stvebx(Thread& t, Instruction op) {
    store_element(t, op, 1);
}
void stvehx(Thread& t, Instruction op) {
    store_element(t, op, 2);
}
void stvewx(Thread& t, Instruction op) {
    store_element(t, op, 4);
}

// lvsl and lvsr build the permute control for an unaligned access at the address: the byte
// indexes sh, sh+1, ..., sh+15 (lvsl) or 16-sh, ..., 31-sh (lvsr), where sh = address & 15.
void lvsl(Thread& t, Instruction op) {
    const u32 shift = vector_address(t, op) & 15;
    Vector result;
    for (u32 i = 0; i < 16; ++i) {
        result.set_byte(i, static_cast<u8>(shift + i));
    }
    set_vd(t, op, result);
}

void lvsr(Thread& t, Instruction op) {
    const u32 shift = vector_address(t, op) & 15;
    Vector result;
    for (u32 i = 0; i < 16; ++i) {
        result.set_byte(i, static_cast<u8>(16 - shift + i));
    }
    set_vd(t, op, result);
}

// Cell-specific load/store left and right, for unaligned quadwords. "Left" covers from the
// address to the end of its aligned quadword; "right" covers from the quadword's start up to
// the address. A left access at p plus a right access at p + 16 together cover the 16 bytes
// at p. UNVERIFIED: no public document defines these formally; this follows the Cell BE
// Programming Handbook's description, usage example and alignment notes (v1.11, A.3.3).
void lvlx(Thread& t, Instruction op) {
    const u64 address = vector_address(t, op);
    const u64 count = 16 - (address & 15);
    Vector result;
    t.memory.read_bytes(address, std::span<u8>(result.bytes.data(), count));
    set_vd(t, op, result);
}

void lvrx(Thread& t, Instruction op) {
    const u64 address = vector_address(t, op);
    const u64 count = address & 15; // aligned: no bytes, and no memory access
    Vector result;
    t.memory.read_bytes(address - count, std::span<u8>(result.bytes.data() + 16 - count, count));
    set_vd(t, op, result);
}

void stvlx(Thread& t, Instruction op) {
    const u64 address = vector_address(t, op);
    const u64 count = 16 - (address & 15);
    t.memory.write_bytes(address, std::span<const u8>(t.state.vr[op.vs()].bytes.data(), count));
}

void stvrx(Thread& t, Instruction op) {
    const u64 address = vector_address(t, op);
    const u64 count = address & 15;
    const Vector& v = t.state.vr[op.vs()];
    t.memory.write_bytes(address - count, std::span<const u8>(v.bytes.data() + 16 - count, count));
}

// Data stream touch hints. The Cell treats these as no-ops (Programming Handbook, table A-8).
void stream_hint(Thread&, Instruction) {}

// ---- Permute and select ----

// vperm: each byte of c picks one of the 32 bytes of a followed by b.
void vperm(Thread& t, Instruction op) {
    const Vector &a = va(t, op), &b = vb(t, op), &c = vc(t, op);
    Vector result;
    for (u32 i = 0; i < 16; ++i) {
        const u32 index = c.byte(i) & 31;
        result.set_byte(i, index < 16 ? a.byte(index) : b.byte(index - 16));
    }
    set_vd(t, op, result);
}

// vsel: bits from b where c is 1, from a where c is 0.
void vsel(Thread& t, Instruction op) {
    Vector result;
    for (u32 i = 0; i < 16; ++i) {
        const u8 mask = vc(t, op).byte(i);
        result.set_byte(i,
                        static_cast<u8>((va(t, op).byte(i) & ~mask) | (vb(t, op).byte(i) & mask)));
    }
    set_vd(t, op, result);
}

// vsldoi: 16 bytes of a followed by b, starting at byte SHB.
void vsldoi(Thread& t, Instruction op) {
    Vector result;
    for (u32 i = 0; i < 16; ++i) {
        const u32 index = op.shb() + i;
        result.set_byte(i, index < 16 ? va(t, op).byte(index) : vb(t, op).byte(index - 16));
    }
    set_vd(t, op, result);
}

// ---- Shift the whole register ----
// The manual requires every byte of b to hold the same shift count; we use the last byte.

// vsl and vsr: shift by 0-7 bits.
void vsl(Thread& t, Instruction op) {
    const u32 n = vb(t, op).byte(15) & 7;
    const Vector& a = va(t, op);
    Vector result;
    for (u32 i = 0; i < 16; ++i) {
        const u32 next = i < 15 ? a.byte(i + 1) : 0;
        result.set_byte(i, static_cast<u8>((a.byte(i) << n) | (next >> (8 - n))));
    }
    set_vd(t, op, result);
}

void vsr(Thread& t, Instruction op) {
    const u32 n = vb(t, op).byte(15) & 7;
    const Vector& a = va(t, op);
    Vector result;
    for (u32 i = 0; i < 16; ++i) {
        const u32 previous = i > 0 ? a.byte(i - 1) : 0;
        result.set_byte(i, static_cast<u8>((a.byte(i) >> n) | (previous << (8 - n))));
    }
    set_vd(t, op, result);
}

// vslo and vsro: shift by whole bytes (octets), 0-15 of them.
void vslo(Thread& t, Instruction op) {
    const u32 n = (vb(t, op).byte(15) >> 3) & 15;
    Vector result;
    for (u32 i = 0; i + n < 16; ++i) {
        result.set_byte(i, va(t, op).byte(i + n));
    }
    set_vd(t, op, result);
}

void vsro(Thread& t, Instruction op) {
    const u32 n = (vb(t, op).byte(15) >> 3) & 15;
    Vector result;
    for (u32 i = n; i < 16; ++i) {
        result.set_byte(i, va(t, op).byte(i - n));
    }
    set_vd(t, op, result);
}

// ---- Merge and splat ----

// vmrgh*: interleave the high halves of a and b; vmrgl*: the low halves.
void merge(Thread& t, Instruction op, Width w, bool low) {
    const u32 half = count(w) / 2;
    Vector result;
    for (u32 i = 0; i < half; ++i) {
        set(result, w, 2 * i, get(va(t, op), w, i + (low ? half : 0)));
        set(result, w, 2 * i + 1, get(vb(t, op), w, i + (low ? half : 0)));
    }
    set_vd(t, op, result);
}

void vmrghb(Thread& t, Instruction op) {
    merge(t, op, Width::byte, false);
}
void vmrghh(Thread& t, Instruction op) {
    merge(t, op, Width::half, false);
}
void vmrghw(Thread& t, Instruction op) {
    merge(t, op, Width::word, false);
}
void vmrglb(Thread& t, Instruction op) {
    merge(t, op, Width::byte, true);
}
void vmrglh(Thread& t, Instruction op) {
    merge(t, op, Width::half, true);
}
void vmrglw(Thread& t, Instruction op) {
    merge(t, op, Width::word, true);
}

// vsplt*: copy one element of b to every element. The element number wraps.
void splat(Thread& t, Instruction op, Width w) {
    const u32 value = get(vb(t, op), w, op.vuimm() % count(w));
    Vector result;
    for (u32 i = 0; i < count(w); ++i) {
        set(result, w, i, value);
    }
    set_vd(t, op, result);
}

void vspltb(Thread& t, Instruction op) {
    splat(t, op, Width::byte);
}
void vsplth(Thread& t, Instruction op) {
    splat(t, op, Width::half);
}
void vspltw(Thread& t, Instruction op) {
    splat(t, op, Width::word);
}

// vspltis*: every element is the sign-extended 5-bit immediate.
void splat_immediate(Thread& t, Instruction op, Width w) {
    Vector result;
    for (u32 i = 0; i < count(w); ++i) {
        set(result, w, i, static_cast<u32>(op.vsimm()));
    }
    set_vd(t, op, result);
}

void vspltisb(Thread& t, Instruction op) {
    splat_immediate(t, op, Width::byte);
}
void vspltish(Thread& t, Instruction op) {
    splat_immediate(t, op, Width::half);
}
void vspltisw(Thread& t, Instruction op) {
    splat_immediate(t, op, Width::word);
}

// ---- Pack ----

// Packs the elements of a then b into elements half their width. `source_signed` says how
// source elements are read; with saturation, `result_signed` picks the range to clamp to.
void pack(Thread& t, Instruction op, Width source, bool saturate, bool source_signed,
          bool result_signed) {
    const Width target = static_cast<Width>(static_cast<u32>(source) / 2);
    const u32 n = count(source);
    bool saturated = false;
    Vector result;
    for (u32 i = 0; i < 2 * n; ++i) {
        const u32 raw = get(i < n ? va(t, op) : vb(t, op), source, i % n);
        u32 value = raw;
        if (saturate) {
            const s64 wide = source_signed ? as_signed(raw, source) : s64{raw};
            value = result_signed ? saturate_signed(wide, target, saturated)
                                  : saturate_unsigned(wide, target, saturated);
        }
        set(result, target, i, value);
    }
    set_vd(t, op, result);
    set_saturated(t, saturated);
}

void vpkuhum(Thread& t, Instruction op) {
    pack(t, op, Width::half, false, false, false);
}
void vpkuwum(Thread& t, Instruction op) {
    pack(t, op, Width::word, false, false, false);
}
void vpkuhus(Thread& t, Instruction op) {
    pack(t, op, Width::half, true, false, false);
}
void vpkuwus(Thread& t, Instruction op) {
    pack(t, op, Width::word, true, false, false);
}
void vpkshus(Thread& t, Instruction op) {
    pack(t, op, Width::half, true, true, false);
}
void vpkswus(Thread& t, Instruction op) {
    pack(t, op, Width::word, true, true, false);
}
void vpkshss(Thread& t, Instruction op) {
    pack(t, op, Width::half, true, true, true);
}
void vpkswss(Thread& t, Instruction op) {
    pack(t, op, Width::word, true, true, true);
}

// vpkpx: packs 32-bit pixels into 1:5:5:5 halfwords, keeping the top bit of each channel byte's
// low part: bit 7 of the word, then the top 5 bits of bytes 1, 2 and 3.
void vpkpx(Thread& t, Instruction op) {
    Vector result;
    for (u32 i = 0; i < 8; ++i) {
        const u32 w = (i < 4 ? va(t, op) : vb(t, op)).word(i % 4);
        const u32 pixel = (((w >> 24) & 1) << 15) | (((w >> 19) & 0x1f) << 10) |
                          (((w >> 11) & 0x1f) << 5) | ((w >> 3) & 0x1f);
        result.set_half(i, static_cast<u16>(pixel));
    }
    set_vd(t, op, result);
}

// ---- Unpack ----

// Sign-extends the high (or low) half of b's elements to double width.
void unpack(Thread& t, Instruction op, Width source, bool low) {
    const Width target = static_cast<Width>(static_cast<u32>(source) * 2);
    const u32 n = count(target);
    Vector result;
    for (u32 i = 0; i < n; ++i) {
        const u32 value = get(vb(t, op), source, i + (low ? n : 0));
        set(result, target, i, static_cast<u32>(as_signed(value, source)));
    }
    set_vd(t, op, result);
}

void vupkhsb(Thread& t, Instruction op) {
    unpack(t, op, Width::byte, false);
}
void vupkhsh(Thread& t, Instruction op) {
    unpack(t, op, Width::half, false);
}
void vupklsb(Thread& t, Instruction op) {
    unpack(t, op, Width::byte, true);
}
void vupklsh(Thread& t, Instruction op) {
    unpack(t, op, Width::half, true);
}

// vupk*px: expands 1:5:5:5 pixels to 8:8:8:8, sign-extending the 1-bit channel.
void unpack_pixels(Thread& t, Instruction op, bool low) {
    Vector result;
    for (u32 i = 0; i < 4; ++i) {
        const u32 pixel = vb(t, op).half(i + (low ? 4 : 0));
        const u32 alpha = (pixel & 0x8000) ? 0xff : 0;
        result.set_word(i, (alpha << 24) | (((pixel >> 10) & 0x1f) << 16) |
                               (((pixel >> 5) & 0x1f) << 8) | (pixel & 0x1f));
    }
    set_vd(t, op, result);
}

void vupkhpx(Thread& t, Instruction op) {
    unpack_pixels(t, op, false);
}
void vupklpx(Thread& t, Instruction op) {
    unpack_pixels(t, op, true);
}

// ---- VSCR ----

void mfvscr(Thread& t, Instruction op) {
    Vector result;
    result.set_word(3, t.state.vscr);
    set_vd(t, op, result);
}

void mtvscr(Thread& t, Instruction op) {
    t.state.vscr = vb(t, op).word(3) & (k_vscr_non_java | k_vscr_saturated);
}

// clang-format off
constexpr OpcodeInfo k_opcodes[] = {
    // mnemonic  form       op  xo    handler      operands
    {"lvx",      Form::X,   31, 103,  lvx,         "vD, rA, rB"},
    {"lvxl",     Form::X,   31, 359,  lvx,         "vD, rA, rB"},
    {"stvx",     Form::X,   31, 231,  stvx,        "vS, rA, rB"},
    {"stvxl",    Form::X,   31, 487,  stvx,        "vS, rA, rB"},
    {"lvebx",    Form::X,   31, 7,    lvebx,       "vD, rA, rB"},
    {"lvehx",    Form::X,   31, 39,   lvehx,       "vD, rA, rB"},
    {"lvewx",    Form::X,   31, 71,   lvewx,       "vD, rA, rB"},
    {"stvebx",   Form::X,   31, 135,  stvebx,      "vS, rA, rB"},
    {"stvehx",   Form::X,   31, 167,  stvehx,      "vS, rA, rB"},
    {"stvewx",   Form::X,   31, 199,  stvewx,      "vS, rA, rB"},
    {"lvsl",     Form::X,   31, 6,    lvsl,        "vD, rA, rB"},
    {"lvsr",     Form::X,   31, 38,   lvsr,        "vD, rA, rB"},
    {"lvlx",     Form::X,   31, 519,  lvlx,        "vD, rA, rB"},
    {"lvlxl",    Form::X,   31, 775,  lvlx,        "vD, rA, rB"},
    {"lvrx",     Form::X,   31, 551,  lvrx,        "vD, rA, rB"},
    {"lvrxl",    Form::X,   31, 807,  lvrx,        "vD, rA, rB"},
    {"stvlx",    Form::X,   31, 647,  stvlx,       "vS, rA, rB"},
    {"stvlxl",   Form::X,   31, 903,  stvlx,       "vS, rA, rB"},
    {"stvrx",    Form::X,   31, 679,  stvrx,       "vS, rA, rB"},
    {"stvrxl",   Form::X,   31, 935,  stvrx,       "vS, rA, rB"},
    {"dst",      Form::X,   31, 342,  stream_hint, "rA, rB, STRM"},
    {"dstst",    Form::X,   31, 374,  stream_hint, "rA, rB, STRM"},
    {"dss",      Form::X,   31, 822,  stream_hint, "STRM"},
    {"vperm",    Form::VA,  4,  43,   vperm,       "vD, vA, vB, vC"},
    {"vsel",     Form::VA,  4,  42,   vsel,        "vD, vA, vB, vC"},
    {"vsldoi",   Form::VA,  4,  44,   vsldoi,      "vD, vA, vB, SHB"},
    {"vsl",      Form::VX,  4,  452,  vsl,         "vD, vA, vB"},
    {"vsr",      Form::VX,  4,  708,  vsr,         "vD, vA, vB"},
    {"vslo",     Form::VX,  4,  1036, vslo,        "vD, vA, vB"},
    {"vsro",     Form::VX,  4,  1100, vsro,        "vD, vA, vB"},
    {"vmrghb",   Form::VX,  4,  12,   vmrghb,      "vD, vA, vB"},
    {"vmrghh",   Form::VX,  4,  76,   vmrghh,      "vD, vA, vB"},
    {"vmrghw",   Form::VX,  4,  140,  vmrghw,      "vD, vA, vB"},
    {"vmrglb",   Form::VX,  4,  268,  vmrglb,      "vD, vA, vB"},
    {"vmrglh",   Form::VX,  4,  332,  vmrglh,      "vD, vA, vB"},
    {"vmrglw",   Form::VX,  4,  396,  vmrglw,      "vD, vA, vB"},
    {"vspltb",   Form::VX,  4,  524,  vspltb,      "vD, vB, UIM"},
    {"vsplth",   Form::VX,  4,  588,  vsplth,      "vD, vB, UIM"},
    {"vspltw",   Form::VX,  4,  652,  vspltw,      "vD, vB, UIM"},
    {"vspltisb", Form::VX,  4,  780,  vspltisb,    "vD, SIM"},
    {"vspltish", Form::VX,  4,  844,  vspltish,    "vD, SIM"},
    {"vspltisw", Form::VX,  4,  908,  vspltisw,    "vD, SIM"},
    {"vpkuhum",  Form::VX,  4,  14,   vpkuhum,     "vD, vA, vB"},
    {"vpkuwum",  Form::VX,  4,  78,   vpkuwum,     "vD, vA, vB"},
    {"vpkuhus",  Form::VX,  4,  142,  vpkuhus,     "vD, vA, vB"},
    {"vpkuwus",  Form::VX,  4,  206,  vpkuwus,     "vD, vA, vB"},
    {"vpkshus",  Form::VX,  4,  270,  vpkshus,     "vD, vA, vB"},
    {"vpkswus",  Form::VX,  4,  334,  vpkswus,     "vD, vA, vB"},
    {"vpkshss",  Form::VX,  4,  398,  vpkshss,     "vD, vA, vB"},
    {"vpkswss",  Form::VX,  4,  462,  vpkswss,     "vD, vA, vB"},
    {"vpkpx",    Form::VX,  4,  782,  vpkpx,       "vD, vA, vB"},
    {"vupkhsb",  Form::VX,  4,  526,  vupkhsb,     "vD, vB"},
    {"vupkhsh",  Form::VX,  4,  590,  vupkhsh,     "vD, vB"},
    {"vupklsb",  Form::VX,  4,  654,  vupklsb,     "vD, vB"},
    {"vupklsh",  Form::VX,  4,  718,  vupklsh,     "vD, vB"},
    {"vupkhpx",  Form::VX,  4,  846,  vupkhpx,     "vD, vB"},
    {"vupklpx",  Form::VX,  4,  974,  vupklpx,     "vD, vB"},
    {"mfvscr",   Form::VX,  4,  1540, mfvscr,      "vD"},
    {"mtvscr",   Form::VX,  4,  1604, mtvscr,      "vB"},
};
// clang-format on

} // namespace

std::span<const OpcodeInfo> vector_permute_opcodes() {
    return k_opcodes;
}

} // namespace ppu
