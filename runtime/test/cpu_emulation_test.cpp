// The Zen 2 instruction emulation (src/cpu_emulation.h, docs/cpu_features.md): the decoder on hand-written
// encodings, the bit-field helpers, `apply` on a synthetic CONTEXT, and then each instruction run for real from an
// executable buffer with the handler installed. On a host that has the feature the CPU runs it and the results
// must be the same; on one that lacks it (an Intel CPU for SSE4a, CLZERO and MONITORX) the handler does the work.
// The test prints which path each instruction took. No game files.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <thread>
#include <vector>

#include "cpu_emulation.h"

namespace ce = rt::cpu_emulation;

namespace {
int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

ce::Decoded dec(std::initializer_list<uint8_t> bytes) {
    std::vector<uint8_t> v(bytes);
    return ce::decode(v.data(), v.size());
}

void test_decode() {
    ce::Decoded d = dec({0xF2, 0x0F, 0x78, 0xC1, 0x10, 0x08});  // insertq xmm0, xmm1, 16, 8
    check(d.op == ce::Op::insertq_imm && d.length == 6 && d.dst == 0 && d.src == 1 && d.len == 16 && d.idx == 8,
          "insertq imm");
    d = dec({0xF2, 0x45, 0x0F, 0x78, 0xD3, 0x05, 0x3B});  // insertq xmm10, xmm11, 5, 59
    check(d.op == ce::Op::insertq_imm && d.length == 7 && d.dst == 10 && d.src == 11 && d.len == 5 && d.idx == 59,
          "insertq imm with REX");
    d = dec({0xF2, 0x0F, 0x79, 0xCA});  // insertq xmm1, xmm2
    check(d.op == ce::Op::insertq_reg && d.length == 4 && d.dst == 1 && d.src == 2, "insertq reg");
    d = dec({0x66, 0x0F, 0x78, 0xC3, 0x08, 0x04});  // extrq xmm3, 8, 4
    check(d.op == ce::Op::extrq_imm && d.length == 6 && d.dst == 3 && d.len == 8 && d.idx == 4, "extrq imm");
    d = dec({0x66, 0x41, 0x0F, 0x78, 0xC7, 0x00, 0x00});  // extrq xmm15, 0, 0
    check(d.op == ce::Op::extrq_imm && d.dst == 15 && d.length == 7, "extrq imm with REX.B");
    d = dec({0x66, 0x0F, 0x79, 0xE9});  // extrq xmm5, xmm1
    check(d.op == ce::Op::extrq_reg && d.dst == 5 && d.src == 1 && d.length == 4, "extrq reg");
    check(dec({0x66, 0x0F, 0x78, 0xC8, 0x08, 0x04}).op == ce::Op::none, "extrq imm needs /0");
    check(dec({0xF2, 0x0F, 0x78, 0x01, 0x08, 0x04}).op == ce::Op::none, "insertq needs a register operand");
    check(dec({0x0F, 0x78, 0xC1}).op == ce::Op::none, "0F 78 without F2/66 is VMREAD, not SSE4a");
    check(dec({0xF3, 0x0F, 0x78, 0xC1, 1, 1}).op == ce::Op::none, "F3 0F 78 is not SSE4a");

    d = dec({0x0F, 0x01, 0xFC});
    check(d.op == ce::Op::clzero && d.length == 3 && !d.addr32, "clzero");
    d = dec({0x67, 0x0F, 0x01, 0xFC});
    check(d.op == ce::Op::clzero && d.length == 4 && d.addr32, "clzero with 67");
    check(dec({0x0F, 0x01, 0xFA}).op == ce::Op::monitorx, "monitorx");
    check(dec({0x0F, 0x01, 0xFB}).op == ce::Op::mwaitx, "mwaitx");
    check(dec({0x0F, 0x01, 0xF9}).op == ce::Op::none, "rdtscp is not handled");
    check(dec({0xF3, 0x0F, 0x01, 0xFA}).op == ce::Op::none, "F3 0F 01 FA is MCOMMIT, not monitorx");
    for (uint8_t pfx : {uint8_t(0x66), uint8_t(0xF2), uint8_t(0xF3), uint8_t(0x48)}) {
        d = dec({pfx, 0x0F, 0x01, 0xFC});
        check(d.op == ce::Op::clzero && d.length == 4, "clzero with 66, F2, F3 or REX (native on Zen 3)");
    }
    d = dec({0x48, 0x66, 0x0F, 0x01, 0xFC});
    check(d.op == ce::Op::clzero && d.length == 5, "clzero with REX before 66 (the REX is ignored)");
    d = dec({0x48, 0x48, 0x0F, 0x01, 0xFC});
    check(d.op == ce::Op::clzero && d.length == 5, "clzero with two REX bytes");
    check(dec({0xF2, 0x0F, 0x01, 0xFB}).op == ce::Op::none, "F2 0F 01 FB is not mwaitx");
    d = dec({0x48, 0x66, 0x0F, 0x78, 0xC1, 1, 1});
    check(d.op == ce::Op::extrq_imm && d.dst == 1, "a REX before 66 does not extend extrq's register");
    d = dec({0x66, 0x0F, 0x01, 0xFC});
    check(d.op == ce::Op::clzero && d.length == 4, "clzero with 66");
    d = dec({0x64, 0x48, 0x0F, 0x01, 0xFC});
    check(d.op == ce::Op::clzero && d.length == 5, "clzero with a segment override and REX");
    check(dec({0xF0, 0x0F, 0x01, 0xFC}).op == ce::Op::none, "LOCK clzero stays #UD");

    check(dec({0x66, 0x0F, 0xAE, 0x31}).length == 4, "clwb [rcx]");
    check(dec({0x66, 0x0F, 0xAE, 0x74, 0x01, 0x08}).length == 6, "clwb [rcx+rax+8]");
    check(dec({0x66, 0x0F, 0xAE, 0xB1, 0, 1, 0, 0}).length == 8, "clwb [rcx+disp32]");
    check(dec({0x66, 0x0F, 0xAE, 0x35, 1, 2, 3, 4}).length == 8, "clwb [rip+disp32]");
    check(dec({0x66, 0x0F, 0xAE, 0x34, 0x25, 1, 2, 3, 4}).length == 9, "clwb [disp32] through SIB");
    check(dec({0x66, 0x41, 0x0F, 0xAE, 0x30}).length == 5 && dec({0x66, 0x41, 0x0F, 0xAE, 0x30}).op == ce::Op::clwb,
          "clwb [r8]");
    check(dec({0x0F, 0xAE, 0x31}).op == ce::Op::none, "0F AE /6 without 66 is xsaveopt");
    check(dec({0x66, 0x0F, 0xAE, 0xF1}).op == ce::Op::none, "66 0F AE /6 with a register is not clwb");
    check(dec({0x66, 0x0F, 0xAE, 0x39}).op == ce::Op::none, "66 0F AE /7 is clflushopt");
    check(dec({0x66, 0x0F, 0xAE, 0x74, 0x01}).op == ce::Op::none, "a cut-off clwb is not decoded");
    check(dec({0x0F, 0x0B}).op == ce::Op::none, "ud2 is not handled");
}

void test_bits() {
    check(ce::insert_bits(0xFFFFFFFFFFFFFFFFull, 0, 8, 4) == 0xFFFFFFFFFFFFF00Full, "insert 8 at 4");
    check(ce::insert_bits(0, 0xABCD, 16, 48) == 0xABCD000000000000ull, "insert 16 at 48");
    check(ce::insert_bits(0x1234, 0xFEDCBA9876543210ull, 0, 0) == 0xFEDCBA9876543210ull, "insert len 0 is 64");
    check(ce::insert_bits(0, 0xFF, 8, 60) == 0xF000000000000000ull, "a field past bit 63 keeps what fits");
    check(ce::extract_bits(0xFEDCBA9876543210ull, 8, 4) == 0x21, "extract 8 at 4");
    check(ce::extract_bits(0xFEDCBA9876543210ull, 0, 0) == 0xFEDCBA9876543210ull, "extract len 0 is 64");
    check(ce::extract_bits(0xFEDCBA9876543210ull, 16, 56) == 0xFE, "extract past bit 63 zero-fills");
}

void test_apply() {
    CONTEXT c{};
    c.Rip = 0x1000;
    c.FltSave.XmmRegisters[0].Low = 0x1111111111111111ull;
    c.FltSave.XmmRegisters[0].High = 0x2222222222222222ll;
    c.FltSave.XmmRegisters[1].Low = 0xAB;
    check(ce::apply(dec({0xF2, 0x0F, 0x78, 0xC1, 0x08, 0x10}), &c), "apply insertq");
    check(c.FltSave.XmmRegisters[0].Low == 0x1111111111AB1111ull && c.Rip == 0x1006, "insertq result and rip");
    check(c.FltSave.XmmRegisters[0].High == 0, "insertq zeroes the upper quadword, as a Zen 3 does");

    c.FltSave.XmmRegisters[2].Low = 0xFFFF;
    c.FltSave.XmmRegisters[2].High = (20ll << 8) | 12;  // len 12 at bits 69:64, idx 20 at bits 77:72 (APM vol. 4)
    c.FltSave.XmmRegisters[3].Low = 0;
    check(ce::apply(dec({0xF2, 0x0F, 0x79, 0xDA}), &c), "apply insertq reg");  // insertq xmm3, xmm2
    check(c.FltSave.XmmRegisters[3].Low == (0xFFFull << 20), "insertq reg takes len and idx from the upper half");

    // Measured on a Zen 3 in #7111's review: control 0x0810 (len 16 in bits 5:0, idx 8 in bits 13:8 of the upper
    // quadword) inserts 16 bits of 0xAB at bit 8 of 0x1234.
    c.FltSave.XmmRegisters[6].Low = 0x1234;
    c.FltSave.XmmRegisters[7].Low = 0xAB;
    c.FltSave.XmmRegisters[7].High = 0x0810;
    check(ce::apply(dec({0xF2, 0x0F, 0x79, 0xF7}), &c) && c.FltSave.XmmRegisters[6].Low == 0xAB34,
          "insertq reg: the measured 0x0810 case");  // insertq xmm6, xmm7

    c.FltSave.XmmRegisters[4].Low = 0xFEDCBA9876543210ull;
    c.FltSave.XmmRegisters[5].Low = (4 << 8) | 8;  // idx 4 at bits 13:8, len 8 at bits 5:0
    check(ce::apply(dec({0x66, 0x0F, 0x79, 0xE5}), &c), "apply extrq reg");  // extrq xmm4, xmm5
    check(c.FltSave.XmmRegisters[4].Low == 0x21, "extrq reg result");

    alignas(64) static uint8_t line[128];
    std::memset(line, 0xEE, sizeof line);
    c.Rax = reinterpret_cast<uint64_t>(line + 64 + 17);
    const uint64_t rip = c.Rip;
    check(ce::apply(dec({0x0F, 0x01, 0xFC}), &c) && c.Rip == rip + 3, "apply clzero");
    bool zeroed = true, kept = true;
    for (int i = 64; i < 128; ++i) zeroed &= line[i] == 0;
    for (int i = 0; i < 64; ++i) kept &= line[i] == 0xEE;
    check(zeroed && kept, "clzero zeroes exactly the line rax points into");

    c.Rax = 0x10;  // the null page: not writable, so the write fault the CPU would raise is reported
    EXCEPTION_RECORD fault{};
    check(!ce::apply(dec({0x0F, 0x01, 0xFC}), &c, &fault) && c.Rip == rip + 3,
          "clzero on an unwritable line is refused and leaves rip");
    check(fault.ExceptionCode == STATUS_ACCESS_VIOLATION && fault.NumberParameters == 2 &&
              fault.ExceptionInformation[0] == 1 && fault.ExceptionInformation[1] == 0,
          "clzero on an unwritable line reports a write access violation at the line");

    // try_clzero_loop EFlags: first-line fault keeps incoming flags, post-prefix fault and >64 MiB batch apply CMP.
    const uint8_t loop12[] = {0x0F, 0x01, 0xFC, 0x48, 0x83, 0xC0, 0x40, 0x48, 0x39, 0xD0, 0x72, 0xF4};
    if (auto* two_pages = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) {
        const uint64_t start = reinterpret_cast<uint64_t>(two_pages);
        DWORD old = 0;
        VirtualProtect(two_pages, 8192, PAGE_READONLY, &old);
        c = {};
        c.Rax = start;
        c.Rdx = start + 8192;
        c.Rip = rip;
        c.EFlags = 0x202;
        check(ce::try_clzero_loop(loop12, sizeof loop12, &c, &fault) == ce::ClzeroLoopResult::faulted &&
                  c.Rax == start && c.Rip == rip && c.EFlags == 0x202,
              "clzero loop fault on first line preserves entry EFlags");

        VirtualProtect(two_pages, 4096, PAGE_READWRITE, &old);
        std::memset(two_pages, 0x5A, 4096);
        c = {};
        c.Rax = start;
        c.Rdx = start + 8192;
        c.Rip = rip;
        c.EFlags = 0x246;  // ZF=1, CF=0 on entry
        check(ce::try_clzero_loop(loop12, sizeof loop12, &c, &fault) == ce::ClzeroLoopResult::faulted,
              "clzero loop post-prefix fault reported");
        bool prefix_zeroed = true;
        for (int i = 0; i < 4096; ++i) prefix_zeroed &= two_pages[i] == 0;
        check(prefix_zeroed && c.Rax == start + 4096 && c.Rip == rip &&
                  fault.ExceptionCode == STATUS_ACCESS_VIOLATION && fault.ExceptionInformation[1] == start + 4096 &&
                  (c.EFlags & 0xC1u) == 0x81u,
              "clzero loop post-prefix fault zeroes prefix and applies CMP flags (CF=1, ZF=0, SF=1)");
        VirtualFree(two_pages, 0, MEM_RELEASE);
    }

    constexpr size_t kLargeSpan = (size_t(64) << 20) + 4096;
    if (auto* large = static_cast<uint8_t*>(VirtualAlloc(nullptr, kLargeSpan, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) {
        const uint64_t base = reinterpret_cast<uint64_t>(large);
        c = {};
        c.Rax = base;
        c.Rdx = base + kLargeSpan;
        c.Rip = rip;
        c.EFlags = 0x202;
        check(ce::try_clzero_loop(loop12, sizeof loop12, &c, &fault) == ce::ClzeroLoopResult::ok &&
                  c.Rip == rip && c.Rax == base + (uint64_t(64) << 20) && (c.EFlags & 0xC1u) == 0x81u,
              "clzero loop >64 MiB first batch applies CMP flags (CF=1, ZF=0, SF=1)");
        check(ce::try_clzero_loop(loop12, sizeof loop12, &c, &fault) == ce::ClzeroLoopResult::ok &&
                  c.Rip == rip + 12 && c.Rax == base + kLargeSpan && (c.EFlags & 0xC1u) == 0x40u,
              "clzero loop >64 MiB second batch completes with CF=0, ZF=1, SF=0");
        VirtualFree(large, 0, MEM_RELEASE);
    }
}

// ---- Running the real encodings ----

using Fn = void (*)(const void* in, void* out);

struct Code {
    uint8_t* mem = nullptr;
    explicit Code(std::initializer_list<uint8_t> bytes) {
        mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (mem) std::memcpy(mem, bytes.begin(), bytes.size());
        FlushInstructionCache(GetCurrentProcess(), mem, 4096);
    }
    ~Code() { if (mem) VirtualFree(mem, 0, MEM_RELEASE); }
    Fn fn() const { return reinterpret_cast<Fn>(mem); }
};

bool run(const Code& code, const void* in, void* out, DWORD* exception) {
    *exception = 0;
    __try {
        code.fn()(in, out);
        return true;
    } __except (*exception = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* path(bool host_has) { return host_has ? "native" : "emulated"; }

void test_live(const ce::Host& host) {
    DWORD ex = 0;
    alignas(32) uint64_t in[8] = {}, out[8] = {};

    // movdqu xmm0,[rcx]; movdqu xmm1,[rcx+16]; insertq xmm0,xmm1,12,20; movdqu [rdx],xmm0; ret
    Code insq({0xF3, 0x0F, 0x6F, 0x01, 0xF3, 0x0F, 0x6F, 0x49, 0x10, 0xF2, 0x0F, 0x78, 0xC1, 12, 20,
               0xF3, 0x0F, 0x7F, 0x02, 0xC3});
    in[0] = 0x0123456789ABCDEFull;
    in[1] = 0x5555555555555555ull;
    in[2] = 0xFFFFFFFFFFFFFABCull;
    check(run(insq, in, out, &ex), "insertq imm ran");
    check(out[0] == ce::insert_bits(in[0], in[2], 12, 20), "insertq imm result");
    std::printf("insertq imm: %s\n", path(host.sse4a));

    // same with the register form: insertq xmm0, xmm1 (len in xmm1[69:64], idx in xmm1[77:72])
    Code insr({0xF3, 0x0F, 0x6F, 0x01, 0xF3, 0x0F, 0x6F, 0x49, 0x10, 0xF2, 0x0F, 0x79, 0xC1,
               0xF3, 0x0F, 0x7F, 0x02, 0xC3});
    in[3] = (7ull << 8) | 33;  // len 33, idx 7
    check(run(insr, in, out, &ex), "insertq reg ran");
    check(out[0] == ce::insert_bits(in[0], in[2], 33, 7), "insertq reg result");

    // movdqu xmm0,[rcx]; extrq xmm0,9,13; movdqu [rdx],xmm0; ret
    Code exti({0xF3, 0x0F, 0x6F, 0x01, 0x66, 0x0F, 0x78, 0xC0, 9, 13, 0xF3, 0x0F, 0x7F, 0x02, 0xC3});
    check(run(exti, in, out, &ex), "extrq imm ran");
    check(out[0] == ce::extract_bits(in[0], 9, 13), "extrq imm result");

    // movdqu xmm0,[rcx]; movdqu xmm1,[rcx+16]; extrq xmm0,xmm1; movdqu [rdx],xmm0; ret
    Code extr({0xF3, 0x0F, 0x6F, 0x01, 0xF3, 0x0F, 0x6F, 0x49, 0x10, 0x66, 0x0F, 0x79, 0xC1,
               0xF3, 0x0F, 0x7F, 0x02, 0xC3});
    in[2] = (40ull << 8) | 20;
    check(run(extr, in, out, &ex), "extrq reg ran");
    check(out[0] == ce::extract_bits(in[0], 20, 40), "extrq reg result");
    std::printf("extrq: %s\n", path(host.sse4a));

    // The upper half of ymm0 survives an emulated (legacy SSE) insertq:
    // vmovdqu ymm0,[rcx]; vmovdqu ymm1,[rcx+32]; insertq xmm0,xmm1,8,8; vmovdqu [rdx],ymm0; vzeroupper; ret
    Code ymm({0xC5, 0xFE, 0x6F, 0x01, 0xC5, 0xFE, 0x6F, 0x49, 0x20, 0xF2, 0x0F, 0x78, 0xC1, 8, 8,
              0xC5, 0xFE, 0x7F, 0x02, 0xC5, 0xF8, 0x77, 0xC3});
    for (int i = 0; i < 8; ++i) in[i] = 0x1010101010101010ull * (i + 1);
    std::memset(out, 0, sizeof out);
    check(run(ymm, in, out, &ex), "insertq between AVX code ran");
    check(out[0] == ce::insert_bits(in[0], in[4], 8, 8), "insertq between AVX code: low quadword");
    check(out[2] == in[2] && out[3] == in[3], "insertq keeps ymm0[255:128]");
    if (!host.sse4a) check(out[1] == 0, "emulated insertq zeroes xmm0[127:64], as a Zen 3 does");

    // mov rax,rcx; clzero; ret
    Code clz({0x48, 0x89, 0xC8, 0x0F, 0x01, 0xFC, 0xC3});
    alignas(64) static uint8_t lines[192];
    std::memset(lines, 0x5A, sizeof lines);
    check(run(clz, lines + 64 + 40, nullptr, &ex), "clzero ran");
    bool ok = true;
    for (int i = 0; i < 192; ++i) ok &= lines[i] == ((i >= 64 && i < 128) ? 0 : 0x5A);
    check(ok, "clzero zeroed one line");
    std::printf("clzero: %s\n", path(host.clzero));

    // A read-only line: the CPU and the handler both raise a write access violation, not #UD.
    if (auto* ro = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))) {
        DWORD old = 0;
        VirtualProtect(ro, 4096, PAGE_READONLY, &old);
        check(!run(clz, ro + 128, nullptr, &ex) && ex == STATUS_ACCESS_VIOLATION,
              "clzero on a read-only line raises an access violation");
        VirtualFree(ro, 0, MEM_RELEASE);
    }
    if (!host.clzero) {  // the emulated path accepts the 66 prefix: mov rax,rcx; 66 clzero; ret
        Code clz66({0x48, 0x89, 0xC8, 0x66, 0x0F, 0x01, 0xFC, 0xC3});
        std::memset(lines, 0x5A, sizeof lines);
        check(run(clz66, lines + 130, nullptr, &ex) && lines[128] == 0 && lines[191] == 0 && lines[127] == 0x5A,
              "clzero with 66 emulated");
    }

    // clzero loop fast-path: mov rax, rcx; clzero; add rax, 0x40; cmp rax, rdx; jb -12; ret
    {
        alignas(64) static uint8_t buf[512];
        std::memset(buf, 0xA5, sizeof buf);
        Code clz_loop({0x48, 0x89, 0xC8, 0x0F, 0x01, 0xFC, 0x48, 0x83, 0xC0, 0x40, 0x48, 0x39, 0xD0, 0x72, 0xF4, 0xC3});
        check(run(clz_loop, buf + 64, buf + 320, &ex), "clzero loop ran");
        bool loop_ok = true;
        for (int i = 0; i < 512; ++i) loop_ok &= buf[i] == ((i >= 64 && i < 320) ? 0 : 0xA5);
        check(loop_ok, "clzero loop zeroed exact span [64, 320)");

        // REX.R limit register (r8): mov rax, rcx; mov r8, rdx; clzero; add rax, 0x40; cmp rax, r8; jb -12; ret
        std::memset(buf, 0xA5, sizeof buf);
        Code clz_r8({0x48, 0x89, 0xC8, 0x49, 0x89, 0xD0, 0x0F, 0x01, 0xFC, 0x48, 0x83, 0xC0, 0x40, 0x4C, 0x39, 0xC0,
                     0x72, 0xF4, 0xC3});
        check(run(clz_r8, buf + 64, buf + 320, &ex), "clzero loop (r8) ran");
        bool r8_ok = true;
        for (int i = 0; i < 512; ++i) r8_ok &= buf[i] == ((i >= 64 && i < 320) ? 0 : 0xA5);
        check(r8_ok, "clzero loop (r8) zeroed exact span [64, 320)");

        // Non-multiple-of-64 limit (300): rounds up to 320, matching native do-while `cmp rax, limit; jb`.
        std::memset(buf, 0xA5, sizeof buf);
        check(run(clz_loop, buf + 64, buf + 300, &ex), "clzero loop (unaligned limit) ran");
        bool unaligned_ok = true;
        for (int i = 0; i < 512; ++i) unaligned_ok &= buf[i] == ((i >= 64 && i < 320) ? 0 : 0xA5);
        check(unaligned_ok, "clzero loop (unaligned limit 300) zeroed span [64, 320)");
    }

    // mov rax,rcx; xor ecx,ecx; xor edx,edx; monitorx; xor eax,eax; xor ecx,ecx; mwaitx; ret
    // (no timer: an interrupt or a store ends a native wait; the emulated one returns at once)
    Code mon({0x48, 0x89, 0xC8, 0x31, 0xC9, 0x31, 0xD2, 0x0F, 0x01, 0xFA, 0x31, 0xC0, 0x31, 0xC9, 0x0F, 0x01, 0xFB,
              0xC3});
    check(run(mon, lines, nullptr, &ex), "monitorx/mwaitx ran");
    std::printf("monitorx/mwaitx: %s\n", path(host.monitorx));

    // xor eax,eax; clwb [rcx]; clwb [rcx+rax+8]; mov dword [rdx],1; ret
    Code wb({0x31, 0xC0, 0x66, 0x0F, 0xAE, 0x31, 0x66, 0x0F, 0xAE, 0x74, 0x01, 0x08, 0xC7, 0x02, 0x01, 0, 0, 0,
             0xC3});
    uint32_t marker = 0;
    check(run(wb, lines, &marker, &ex) && marker == 1, "clwb ran and execution continued after it");
    std::printf("clwb: %s\n", path(host.clwb));
    if (!host.clwb) {
        // Both sites were rewritten at their first #UD: CLFLUSHOPT (reg field 7) or the same-length NOP.
        const auto rewritten = [&](int at) {
            return (wb.mem[at] == 0xAE && (wb.mem[at + 1] & 0x38) == 0x38) ||
                   (wb.mem[at] == 0x1F && (wb.mem[at + 1] & 0x38) == 0);
        };
        check(rewritten(4) && rewritten(8), "clwb sites rewritten in place");
        check(wb.mem[10] == 0x01 && wb.mem[11] == 0x08, "the rewritten clwb keeps its SIB and displacement");
        marker = 0;
        check(run(wb, lines, &marker, &ex) && marker == 1, "the rewritten code runs again");
    }

    // A #UD the handler does not know stays an illegal-instruction exception: ud2; ret
    Code ud({0x0F, 0x0B, 0xC3});
    check(!run(ud, nullptr, nullptr, &ex) && ex == STATUS_ILLEGAL_INSTRUCTION, "ud2 still faults");
}

// ---- CLWB rewritten in place, with the handler called on a made-up #UD ----
// The #UD a CPU without CLWB raises is handed to the handler directly, so this runs on a host with CLWB too. A
// thread whose #UD is handled after another thread rewrote the same site reads the new bytes: it must continue at
// the site (the CPU runs the rewritten instruction), never pass the exception on. Each site is rewritten once.

// The handler's answer to a #UD at `rip`, and the RIP it continues at; -2 when the handler itself faulted.
LONG fake_ud(uint64_t rip, uint64_t* rip_after) {
    EXCEPTION_RECORD record{};
    record.ExceptionCode = DWORD(STATUS_ILLEGAL_INSTRUCTION);
    record.ExceptionAddress = reinterpret_cast<void*>(rip);
    CONTEXT context{};
    context.Rip = rip;
    EXCEPTION_POINTERS pointers{&record, &context};
    __try {
        const LONG r = ce::handler(&pointers);
        *rip_after = context.Rip;
        return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

bool call_at(const uint8_t* code, const void* in) {
    __try {
        reinterpret_cast<Fn>(const_cast<uint8_t*>(code))(in, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// clwb [rcx]; ret at `at` became CLFLUSHOPT [rcx] (reg field 7) or the NOP 0F 1F /0 with the same operand.
bool clwb_rewritten(const uint8_t* at) {
    return at[0] == 0x66 && at[1] == 0x0F && at[4] == 0xC3 && (at[3] & 0xC7) == 0x01 &&
           ((at[2] == 0xAE && (at[3] & 0x38) == 0x38) || (at[2] == 0x1F && (at[3] & 0x38) == 0));
}

bool executable_read_only(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    return VirtualQuery(p, &mbi, sizeof mbi) && mbi.Protect == PAGE_EXECUTE_READ;
}

void test_clwb_rewrite() {
    char value[4]{};
    if (GetEnvironmentVariableA("LEONIDA_CLWB_PATCH", value, sizeof(value)) > 0 && value[0] == '0') {
        std::printf("clwb rewrite: skipped (LEONIDA_CLWB_PATCH=0)\n");
        return;
    }
    constexpr int kSites = 2048, kStride = 8, kThreads = 16;
    const std::size_t bytes = std::size_t(kSites + 1) * kStride;
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    check(mem != nullptr, "clwb rewrite: allocate the sites");
    if (!mem) return;
    for (int s = 0; s <= kSites; ++s) {  // clwb [rcx]; ret; int3 x3
        const uint8_t site[kStride] = {0x66, 0x0F, 0xAE, 0x31, 0xC3, 0xCC, 0xCC, 0xCC};
        std::memcpy(mem + std::size_t(s) * kStride, site, kStride);
    }
    DWORD old = 0;
    VirtualProtect(mem, bytes, PAGE_EXECUTE_READ, &old);  // as the game's code: the handler must unprotect it
    FlushInstructionCache(GetCurrentProcess(), mem, bytes);

    // (a) The first #UD rewrites the site and continues at it, which then runs on any host.
    uint8_t* const one = mem + std::size_t(kSites) * kStride;
    const uint64_t rip = reinterpret_cast<uint64_t>(one);
    const unsigned before = ce::clwb_sites().load();
    uint64_t after = 0;
    check(fake_ud(rip, &after) == EXCEPTION_CONTINUE_EXECUTION && after == rip,
          "clwb rewrite: the first #UD continues at the site");
    check(clwb_rewritten(one), "clwb rewrite: the site is rewritten, operand kept");
    check(ce::clwb_sites().load() == before + 1, "clwb rewrite: the site is counted once");
    check(executable_read_only(one), "clwb rewrite: the page is execute-read again");
    alignas(64) static uint8_t line[64];
    check(call_at(one, line), "clwb rewrite: the rewritten site runs");

    // (b) A #UD handled after the site was rewritten (another thread's, in the game) continues at the site too.
    after = 0;
    check(fake_ud(rip, &after) == EXCEPTION_CONTINUE_EXECUTION && after == rip,
          "clwb rewrite: a #UD on an already rewritten site continues at it");
    check(ce::clwb_sites().load() == before + 1, "clwb rewrite: an already rewritten site is not counted again");

    // (c) 16 threads take a #UD on each of 2048 fresh sites at once, in the same order so they meet on each site.
    std::atomic<int> waiting{kThreads}, bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&] {
            waiting.fetch_sub(1);
            while (waiting.load()) std::this_thread::yield();
            for (int s = 0; s < kSites; ++s) {
                const uint64_t at = reinterpret_cast<uint64_t>(mem + std::size_t(s) * kStride);
                uint64_t continued = 0;
                if (fake_ud(at, &continued) != EXCEPTION_CONTINUE_EXECUTION || continued != at) bad.fetch_add(1);
            }
        });
    for (auto& t : threads) t.join();
    if (bad.load()) std::fprintf(stderr, "clwb rewrite: %d of %d concurrent #UDs not continued at the site\n",
                                 bad.load(), kThreads * kSites);
    check(bad.load() == 0, "clwb rewrite: every concurrent #UD continues at its site");
    const unsigned counted = ce::clwb_sites().load() - before - 1;
    if (counted != unsigned(kSites)) std::fprintf(stderr, "clwb rewrite: %u sites counted for %d\n", counted, kSites);
    check(counted == unsigned(kSites), "clwb rewrite: each concurrently hit site is counted once");
    bool all = true;
    for (int s = 0; s < kSites; ++s) all &= clwb_rewritten(mem + std::size_t(s) * kStride);
    check(all, "clwb rewrite: every concurrently hit site is rewritten");
    check(executable_read_only(mem) && executable_read_only(mem + bytes - 1),
          "clwb rewrite: the pages are execute-read again");
    std::printf("clwb rewrite: %d threads x %d sites\n", kThreads, kSites);
    VirtualFree(mem, 0, MEM_RELEASE);
}
}  // namespace

int main() {
    test_decode();
    test_bits();
    test_apply();
    const ce::Host host = ce::detect_host();
    std::printf("host: SSE4a %d, MONITORX %d, CLZERO %d, CLWB %d\n", host.sse4a, host.monitorx, host.clzero,
                host.clwb);
    check(ce::install(), "install the handler");
    test_live(host);
    test_clwb_rewrite();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("cpu_emulation: all checks passed\n");
    return 0;
}
