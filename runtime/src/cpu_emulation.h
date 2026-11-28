// Emulates the few Zen 2 instructions in the game's code that a desktop CPU may lack (docs/cpu_features.md).
//
// The executable is built for the console's AMD Zen 2 and uses some of its instructions without asking CPUID:
// SSE4a INSERTQ/EXTRQ, CLZERO and MONITORX/MWAITX (AMD only) and CLWB (Intel since Ice Lake). On a host without
// one, the CPU raises #UD (STATUS_ILLEGAL_INSTRUCTION) at that instruction. A vectored handler recognises these
// encodings at the faulting RIP, does the same work on the thread's CONTEXT and continues after the instruction.
// On a host that has the feature the CPU runs the instruction itself and the handler never sees it.
//
// What each one does, from the AMD64 Architecture Programmer's Manual (vol. 3 for CLZERO, MONITORX, MWAITX; vol. 4
// for INSERTQ, EXTRQ) and the Intel SDM vol. 2 for CLWB:
//   INSERTQ xmm1, xmm2, len, idx   F2 0F 78 /r ib ib   insert the low `len` bits of xmm2[63:0] into xmm1[63:0] at
//                                                       bit `idx` (len 0 means 64)
//   INSERTQ xmm1, xmm2             F2 0F 79 /r         same, len = xmm2[69:64], idx = xmm2[77:72]
//   EXTRQ   xmm1, len, idx         66 0F 78 /0 ib ib   xmm1[63:0] = (xmm1[63:0] >> idx), `len` bits, zero-extended
//   EXTRQ   xmm1, xmm2             66 0F 79 /r         same, len = xmm2[5:0], idx = xmm2[13:8]
//   CLZERO                         0F 01 FC            zero the 64-byte cache line that rAX points into; a line
//                                                       it cannot write raises that write fault, as the CPU does
//   MONITORX                       0F 01 FA            arm an address monitor: a hint, nothing to do here
//   MWAITX                         0F 01 FB            wait for the monitor or a timer; returning at once is one
//                                                       of the exits the manual allows, so it is a pause
//   CLWB m8                        66 0F AE /6 (mem)   write a line back, keeping it cached: ordinary write-back
//                                                       memory is coherent, so there is nothing to do here
// The APM leaves xmm1[127:64] undefined after INSERTQ and EXTRQ; a Zen 3 zeroes it in all four forms (measured in
// #7111's review), so the emulation does too. A field with len + idx > 64 is undefined as well; the emulation keeps
// the bits that fit, which matched that Zen 3 on every len/idx pair.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rt::cpu_emulation {

enum class Op { none, insertq_imm, insertq_reg, extrq_imm, extrq_reg, clzero, monitorx, mwaitx, clwb };

struct Decoded {
    Op op = Op::none;
    std::size_t length = 0;  // bytes, prefixes included
    int dst = 0;             // xmm register (INSERTQ, EXTRQ)
    int src = 0;             // xmm register (the register forms)
    uint8_t len = 0;         // field length and index of the immediate forms
    uint8_t idx = 0;
    bool addr32 = false;     // a 67 prefix: CLZERO uses EAX
};

// One of the encodings above at `code` (`avail` readable bytes), or Op::none.
inline Decoded decode(const uint8_t* code, std::size_t avail) {
    Decoded d;
    std::size_t i = 0;
    bool p66 = false, pf2 = false, pf3 = false;
    uint8_t rex = 0;
    // Legacy prefixes and REX bytes in any order; only a REX right before the opcode counts, an earlier one is
    // ignored (as AMD CPUs do: `48 66 0F 01 FC` and `48 48 0F 01 FC` are CLZERO). LOCK (F0) ends the scan: #UD.
    for (; i < avail && i < 14; ++i) {
        const uint8_t b = code[i];
        if ((b & 0xF0) == 0x40) { rex = b; continue; }
        if (b == 0x66) p66 = true;
        else if (b == 0xF2) pf2 = true;
        else if (b == 0xF3) pf3 = true;
        else if (b == 0x67) d.addr32 = true;
        else if (b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 || b == 0x64 || b == 0x65) {}
        else break;
        rex = 0;
    }
    if (i + 2 > avail || code[i] != 0x0F) return {};
    const uint8_t op = code[i + 1];
    i += 2;
    if (op == 0x01) {  // CLZERO, MONITORX, MWAITX
        if (i >= avail) return {};
        const uint8_t m = code[i++];
        // AMD CPUs run CLZERO with 66, F2 or F3 in front (measured on Zen 3 in #7055's reviews). For FA/FB an F3
        // selects another instruction (F3 0F 01 FA is MCOMMIT), so those stay #UD with F2/F3.
        if ((pf2 || pf3) && m != 0xFC) return {};
        d.op = m == 0xFC ? Op::clzero : m == 0xFA ? Op::monitorx : m == 0xFB ? Op::mwaitx : Op::none;
        d.length = i;
        return d.op == Op::none ? Decoded{} : d;
    }
    if (i >= avail) return {};
    const uint8_t modrm = code[i++];
    const int mod = modrm >> 6, reg = ((modrm >> 3) & 7) | ((rex & 4) << 1), rm = (modrm & 7) | ((rex & 1) << 3);
    if (op == 0x78 || op == 0x79) {  // SSE4a; F2 is INSERTQ, 66 is EXTRQ; both need a register operand
        if (mod != 3 || pf3 || p66 == pf2) return {};
        const bool imm = op == 0x78;
        if (imm) {
            if (i + 2 > avail) return {};
            if (p66 && (modrm & 0x38)) return {};  // EXTRQ's immediate form is /0
            d.len = code[i] & 0x3F;
            d.idx = code[i + 1] & 0x3F;
            i += 2;
        }
        d.op = pf2 ? (imm ? Op::insertq_imm : Op::insertq_reg) : (imm ? Op::extrq_imm : Op::extrq_reg);
        if (p66 && imm) d.dst = rm;  // EXTRQ xmm1, imm, imm names xmm1 in r/m
        else d.dst = reg, d.src = rm;
        d.length = i;
        return d;
    }
    if (op == 0xAE && p66 && !pf2 && !pf3 && mod != 3 && ((modrm >> 3) & 7) == 6) {  // CLWB m8
        if ((modrm & 7) == 4) {  // SIB
            if (i >= avail) return {};
            const uint8_t sib = code[i++];
            if (mod == 0 && (sib & 7) == 5) i += 4;
        } else if (mod == 0 && (modrm & 7) == 5) {
            i += 4;  // RIP-relative
        }
        i += mod == 1 ? 1 : mod == 2 ? 4 : 0;
        if (i > avail) return {};
        d.op = Op::clwb;
        d.length = i;
        return d;
    }
    return {};
}

inline uint64_t field_mask(unsigned len) { return len >= 64 ? ~uint64_t(0) : (uint64_t(1) << len) - 1; }

// INSERTQ on the low quadword: `len` (0 means 64) bits of `src` placed at bit `idx` of `dst`.
inline uint64_t insert_bits(uint64_t dst, uint64_t src, unsigned len, unsigned idx) {
    len &= 63;
    idx &= 63;
    const uint64_t mask = field_mask(len ? len : 64) << idx;
    return (dst & ~mask) | ((src << idx) & mask);
}

// EXTRQ on the low quadword: `len` (0 means 64) bits of `src` from bit `idx`, zero-extended.
inline uint64_t extract_bits(uint64_t src, unsigned len, unsigned idx) {
    len &= 63;
    idx &= 63;
    return (src >> idx) & field_mask(len ? len : 64);
}

}  // namespace rt::cpu_emulation

#ifdef _WIN32
#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>

namespace rt::cpu_emulation {

struct Host {
    bool sse4a = false, monitorx = false, clzero = false, clwb = false;
};

inline Host detect_host() {
    Host h;
    int r[4];
    __cpuid(r, 0);
    const int max_leaf = r[0];
    if (max_leaf >= 7) {
        __cpuidex(r, 7, 0);
        h.clwb = (r[1] >> 24) & 1;  // EBX bit 24
    }
    __cpuid(r, 0x80000000);
    const unsigned max_ext = unsigned(r[0]);
    if (max_ext >= 0x80000001) {
        __cpuid(r, 0x80000001);
        h.sse4a = (r[2] >> 6) & 1;      // ECX bit 6
        h.monitorx = (r[2] >> 29) & 1;  // ECX bit 29
    }
    if (max_ext >= 0x80000008) {
        __cpuid(r, 0x80000008);
        h.clzero = r[1] & 1;  // EBX bit 0
    }
    return h;
}

inline const char* name(Op op) {
    switch (op) {
    case Op::insertq_imm: case Op::insertq_reg: return "INSERTQ (SSE4a)";
    case Op::extrq_imm: case Op::extrq_reg: return "EXTRQ (SSE4a)";
    case Op::clzero: return "CLZERO";
    case Op::monitorx: return "MONITORX";
    case Op::mwaitx: return "MWAITX";
    case Op::clwb: return "CLWB";
    default: return "none";
    }
}

// How many times each instruction was emulated; the two forms of INSERTQ and of EXTRQ share one count.
inline std::atomic<uint64_t>& count(Op op) {
    static std::atomic<uint64_t> counts[9];
    if (op == Op::insertq_reg) op = Op::insertq_imm;
    if (op == Op::extrq_reg) op = Op::extrq_imm;
    return counts[int(op)];
}

// 0 when the page holding `addr` can be written; otherwise the exception a write raises there. A guard page loses
// its guard on that first touch and reports STATUS_GUARD_PAGE_VIOLATION, as the system does for a real access.
inline DWORD writable(uint64_t addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)
        return DWORD(STATUS_ACCESS_VIOLATION);
    if (mbi.Protect & PAGE_GUARD) {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(addr & ~uint64_t(0xFFF)), 1, mbi.Protect & ~DWORD(PAGE_GUARD), &old);
        return DWORD(STATUS_GUARD_PAGE_VIOLATION);
    }
    const DWORD p = mbi.Protect & 0xFF;
    const bool ok = p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE ||
                    p == PAGE_EXECUTE_WRITECOPY;
    return ok ? 0 : DWORD(STATUS_ACCESS_VIOLATION);
}

// How many of the `want` bytes at `addr` can be read, by page protection (no fault is taken to find out).
inline std::size_t readable(uint64_t addr, std::size_t want) {
    std::size_t got = 0;
    while (got < want) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(addr + got), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !mbi.Protect)
            break;
        const uint64_t end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
        got += std::size_t(end - (addr + got)) < want - got ? std::size_t(end - (addr + got)) : want - got;
    }
    return got;
}

inline uint64_t& lo(CONTEXT* c, int r) { return c->FltSave.XmmRegisters[r].Low; }
inline uint64_t hi(const CONTEXT* c, int r) { return uint64_t(c->FltSave.XmmRegisters[r].High); }

// Apply one decoded instruction to `c` and step past it. False when the line CLZERO zeroes cannot be written; then
// `fault`, when given, receives the exception the write raised (STATUS_ACCESS_VIOLATION with the write flag and the
// line's address, or STATUS_GUARD_PAGE_VIOLATION, as the real instruction would raise) and `c` is unchanged.
inline bool apply(const Decoded& d, CONTEXT* c, EXCEPTION_RECORD* fault = nullptr) {
    switch (d.op) {
    case Op::insertq_imm:
        lo(c, d.dst) = insert_bits(lo(c, d.dst), lo(c, d.src), d.len, d.idx);
        c->FltSave.XmmRegisters[d.dst].High = 0;
        break;
    case Op::insertq_reg: {
        const uint64_t ctl = hi(c, d.src);
        lo(c, d.dst) = insert_bits(lo(c, d.dst), lo(c, d.src), unsigned(ctl) & 63, unsigned(ctl >> 8) & 63);
        c->FltSave.XmmRegisters[d.dst].High = 0;
        break;
    }
    case Op::extrq_imm:
        lo(c, d.dst) = extract_bits(lo(c, d.dst), d.len, d.idx);
        c->FltSave.XmmRegisters[d.dst].High = 0;
        break;
    case Op::extrq_reg: {
        const uint64_t ctl = lo(c, d.src);
        lo(c, d.dst) = extract_bits(lo(c, d.dst), unsigned(ctl) & 63, unsigned(ctl >> 8) & 63);
        c->FltSave.XmmRegisters[d.dst].High = 0;
        break;
    }
    case Op::clzero: {
        const uint64_t addr = (d.addr32 ? (c->Rax & 0xFFFFFFFFu) : c->Rax) & ~uint64_t(63);
        // The line's page is checked first instead of catching a fault from the write: a compiler may turn the
        // write into inline stores that a local __try does not cover (clang-cl without /EHa). A 64-byte aligned
        // line never spans two pages, so one query answers for all of it.
        const DWORD code = writable(addr);
        if (code) {
            if (fault) {
                std::memset(fault, 0, sizeof *fault);
                fault->ExceptionCode = code;
                fault->NumberParameters = 2;
                fault->ExceptionInformation[0] = 1;  // a write
                fault->ExceptionInformation[1] = ULONG_PTR(addr);
            }
            return false;
        }
        std::memset(reinterpret_cast<void*>(addr), 0, 64);
        break;
    }
    case Op::mwaitx: YieldProcessor(); break;
    case Op::monitorx: case Op::clwb: break;
    default: return false;
    }
    c->Rip += d.length;
    return true;
}

inline uint64_t gpr_read(const CONTEXT* c, int r) {
    switch (r) {
    case 0: return c->Rax;   case 1: return c->Rcx;   case 2: return c->Rdx;   case 3: return c->Rbx;
    case 4: return c->Rsp;   case 5: return c->Rbp;   case 6: return c->Rsi;   case 7: return c->Rdi;
    case 8: return c->R8;    case 9: return c->R9;    case 10: return c->R10;  case 11: return c->R11;
    case 12: return c->R12;  case 13: return c->R13;  case 14: return c->R14;  case 15: return c->R15;
    default: return 0;
    }
}

enum class ClzeroLoopResult { no_match, ok, faulted };

inline void apply_cmp_flags(CONTEXT* c, uint64_t lhs, uint64_t rhs) {
    const uint64_t diff = lhs - rhs;
    uint32_t flags = c->EFlags & ~DWORD(0x8D5u);  // clear CF(0), PF(2), AF(4), ZF(6), SF(7), OF(11)
    if (lhs < rhs) flags |= 0x1u;                         // CF
    uint8_t lo8 = static_cast<uint8_t>(diff);
    lo8 ^= lo8 >> 4;
    lo8 ^= lo8 >> 2;
    lo8 ^= lo8 >> 1;
    if ((lo8 & 1u) == 0) flags |= 0x4u;                   // PF
    if ((lhs ^ rhs ^ diff) & 0x10u) flags |= 0x10u;       // AF
    if (diff == 0) flags |= 0x40u;                        // ZF
    if (diff >> 63) flags |= 0x80u;                       // SF
    if (((lhs ^ rhs) & (lhs ^ diff)) >> 63) flags |= 0x800u;  // OF
    c->EFlags = flags;
}

// Fast-path for the Zen 2 zeroing loop: `clzero; add rax, 0x40; cmp rax, reg64; jb -12` (12 bytes).
// Zeroes the entire writable span in one memset instead of raising #UD + VirtualQuery per 64-byte line.
inline ClzeroLoopResult try_clzero_loop(const uint8_t* code, std::size_t avail, CONTEXT* c, EXCEPTION_RECORD* fault) {
    if (avail < 12) return ClzeroLoopResult::no_match;
    if (code[0] != 0x0F || code[1] != 0x01 || code[2] != 0xFC) return ClzeroLoopResult::no_match;
    if (code[3] != 0x48 || code[4] != 0x83 || code[5] != 0xC0 || code[6] != 0x40) return ClzeroLoopResult::no_match;
    if ((code[7] != 0x48 && code[7] != 0x4C) || code[8] != 0x39 || (code[9] & 0xC7) != 0xC0)
        return ClzeroLoopResult::no_match;
    if (code[10] != 0x72 || code[11] != 0xF4) return ClzeroLoopResult::no_match;

    const int limit_reg = ((code[9] >> 3) & 7) | ((code[7] & 4) << 1);
    if (limit_reg == 0 || limit_reg == 4) return ClzeroLoopResult::no_match;

    const uint64_t start = c->Rax;
    const uint64_t limit = gpr_read(c, limit_reg);
    if ((start & 63) != 0 || limit <= start + 64) return ClzeroLoopResult::no_match;

    uint64_t total = (limit - start + 63) & ~uint64_t(63);
    constexpr uint64_t kMaxBatch = uint64_t(64) << 20;
    if (total > kMaxBatch) total = kMaxBatch;
    const uint64_t end = start + total;

    for (uint64_t cur = start; cur < end;) {
        const DWORD err = writable(cur);
        if (err) {
            if (cur > start) {
                std::memset(reinterpret_cast<void*>(start), 0, std::size_t(cur - start));
                c->Rax = cur;
                apply_cmp_flags(c, cur, limit);
            }
            if (fault) {
                std::memset(fault, 0, sizeof *fault);
                fault->ExceptionCode = err;
                fault->NumberParameters = 2;
                fault->ExceptionInformation[0] = 1;
                fault->ExceptionInformation[1] = ULONG_PTR(cur);
            }
            return ClzeroLoopResult::faulted;
        }
        const uint64_t next_page = (cur & ~uint64_t(0xFFF)) + 0x1000;
        cur = next_page < end ? next_page : end;
    }

    std::memset(reinterpret_cast<void*>(start), 0, std::size_t(total));
    c->Rax = end;
    apply_cmp_flags(c, end, limit);
    if (end >= limit) c->Rip += 12;
    return ClzeroLoopResult::ok;
}

// CLWB is rewritten in place at its first #UD, so a loop over a buffer faults once instead of once per cache line.
// The retail title's GPU upload worker flushes whole buffers that way (exe+0x532f1b0 on 1.0.2.1); under Wine a #UD
// costs tens of microseconds, which kept that worker busy for minutes per buffer and the boot waiting on it.
// 66 0F AE /6 (CLWB) becomes 66 0F AE /7 (CLFLUSHOPT): the same operand and length, and it writes the line back too
// (then evicts it). A host without CLFLUSHOPT gets 66 0F 1F /0, a NOP of the same length that touches no memory.
// LEONIDA_CLWB_PATCH=0 keeps emulating every execution instead.
// Threads of that worker reach the same site together: a #UD handled after another thread rewrote the site finds
// the new instruction there and continues at it, and the rewrite itself is done once, under a lock.

// 1: rewrite as CLFLUSHOPT, 2: as the NOP (no CLFLUSHOPT on this host), 0: LEONIDA_CLWB_PATCH=0.
inline int clwb_patch_mode() {
    static const int mode = [] {
        char value[4]{};
        if (GetEnvironmentVariableA("LEONIDA_CLWB_PATCH", value, sizeof(value)) > 0 && value[0] == '0') return 0;
        int r[4];
        __cpuidex(r, 7, 0);
        return ((r[1] >> 23) & 1) ? 1 : 2;  // EBX bit 23: CLFLUSHOPT
    }();
    return mode;
}

// How many CLWB sites were rewritten so far; each counts once, by the thread that wrote it.
inline std::atomic<unsigned>& clwb_sites() {
    static std::atomic<unsigned> sites{0};
    return sites;
}

// True when `code` (`avail` bytes at a #UD) holds the form patch_clwb writes on this host, or the NOP's halfway
// state (66 0F 1F /6): another thread rewrote the site after this thread's #UD. Neither form raises #UD here, so the
// thread continues at the site and the CPU runs it.
inline bool clwb_rewritten(const uint8_t* code, std::size_t avail) {
    const int mode = clwb_patch_mode();
    if (!mode) return false;
    std::size_t i = 0;
    bool p66 = false;
    for (; i < avail && i < 14; ++i) {
        const uint8_t b = code[i];
        if (b == 0x66) p66 = true;
        else if (!((b & 0xF0) == 0x40 || b == 0x67 || b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 ||
                   b == 0x64 || b == 0x65)) break;
    }
    if (!p66 || i + 3 > avail || code[i] != 0x0F || (code[i + 2] >> 6) == 3) return false;
    const int reg = (code[i + 2] >> 3) & 7;
    return mode == 1 ? code[i + 1] == 0xAE && reg == 7 : code[i + 1] == 0x1F && (reg == 0 || reg == 6);
}

inline bool patch_clwb(uint64_t rip, const uint8_t* code, std::size_t length) {
    const int mode = clwb_patch_mode();
    if (!mode) return false;
    std::size_t at = 0;
    while (at + 2 < length && !(code[at] == 0x0F && code[at + 1] == 0xAE)) ++at;
    if (at + 2 >= length) return false;
    const uint8_t opcode = mode == 1 ? 0xAE : 0x1F;
    const uint8_t modrm = mode == 1 ? uint8_t(code[at + 2] | 0x08) : uint8_t(code[at + 2] & ~0x38);  // reg 7 or 0
    void* const where = reinterpret_cast<void*>(rip + at);
    volatile uint8_t* const live = static_cast<volatile uint8_t*>(where);
    // One thread at a time unprotects, checks, writes and protects again: without the lock a thread could write into
    // a page another one has just set back to read-execute, or set it back to the read-write-execute it found.
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    bool ok = live[1] == opcode && live[2] == modrm;  // another thread rewrote it since this thread's #UD
    bool wrote = false;
    DWORD old = 0;
    if (!ok && live[0] == 0x0F && live[1] == 0xAE && live[2] == code[at + 2] &&
        VirtualProtect(where, 3, PAGE_EXECUTE_READWRITE, &old)) {
        // Opcode first: between the two stores a thread running the site sees 66 0F 1F /6, a NOP as well. ModRM first
        // would show it 66 0F AE /0, FXSAVE's encoding with a 66 the SDM does not allow (a 512-byte store at the
        // operand, or a #UD that is no longer CLWB). CLFLUSHOPT only changes ModRM, one store.
        if (live[1] != opcode) live[1] = opcode;
        live[2] = modrm;
        VirtualProtect(where, 3, old, &old);
        FlushInstructionCache(GetCurrentProcess(), where, 3);
        ok = wrote = true;
    }
    ReleaseSRWLockExclusive(&lock);
    if (wrote) {
        const unsigned n = ++clwb_sites();
        if (n <= 4 || !(n & (n - 1)))
            std::fprintf(stderr, "[rt] cpu: CLWB at %p rewritten as %s (%u sites so far)\n",
                         reinterpret_cast<void*>(rip), mode == 1 ? "CLFLUSHOPT" : "a NOP", n);
    }
    return ok;
}

inline LONG CALLBACK handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != STATUS_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = info->ContextRecord;
    uint8_t code[15] = {};
    const std::size_t avail = readable(c->Rip, sizeof code);  // the instruction may end at a page boundary
    std::memcpy(code, reinterpret_cast<const void*>(c->Rip), avail);
    const Decoded d = decode(code, avail);
    if (d.op == Op::none)
        return clwb_rewritten(code, avail) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    if (d.op == Op::clwb && patch_clwb(c->Rip, code, d.length)) return EXCEPTION_CONTINUE_EXECUTION;
    EXCEPTION_RECORD fault;
    if (d.op == Op::clzero && !d.addr32 && d.length == 3) {
        const uint64_t fault_rip = c->Rip;
        const ClzeroLoopResult lr = try_clzero_loop(code, avail, c, &fault);
        if (lr == ClzeroLoopResult::ok) {
            if (count(d.op).fetch_add(1, std::memory_order_relaxed) == 0)
                std::fprintf(stderr, "[rt] cpu: this CPU has no %s; emulating it (first at %p)\n", name(d.op),
                             reinterpret_cast<void*>(fault_rip));
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (lr == ClzeroLoopResult::faulted) {
            EXCEPTION_RECORD* r = info->ExceptionRecord;
            r->ExceptionCode = fault.ExceptionCode;
            r->ExceptionFlags = 0;
            r->NumberParameters = fault.NumberParameters;
            r->ExceptionInformation[0] = fault.ExceptionInformation[0];
            r->ExceptionInformation[1] = fault.ExceptionInformation[1];
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    if (!apply(d, c, &fault)) {
        // CLZERO on a line it cannot write: the CPU would raise that fault at this instruction, so the next handlers
        // see it instead of the #UD (RIP still points at the instruction).
        EXCEPTION_RECORD* r = info->ExceptionRecord;
        r->ExceptionCode = fault.ExceptionCode;
        r->ExceptionFlags = 0;
        r->NumberParameters = fault.NumberParameters;
        r->ExceptionInformation[0] = fault.ExceptionInformation[0];
        r->ExceptionInformation[1] = fault.ExceptionInformation[1];
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (count(d.op).fetch_add(1, std::memory_order_relaxed) == 0)
        std::fprintf(stderr, "[rt] cpu: this CPU has no %s; emulating it (first at %p)\n", name(d.op),
                     reinterpret_cast<void*>(c->Rip - d.length));
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Installs the handler first in the vectored chain, once. Returns false if Windows refused it.
inline bool install() {
    static void* const cookie = AddVectoredExceptionHandler(1, &handler);
    return cookie != nullptr;
}

}  // namespace rt::cpu_emulation
#endif
