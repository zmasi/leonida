"""GPU test of the address guard (rdna2spirv.py --address-guard; docs/d3d12_vulkan.md, "Address guard").

Assembles runtime/test/shaders/cs_address_guard.asm, translates it with --address-guard and runs it with
vk_compute_test twice on a 2 MiB heap: once with the guard's header (the page table backs the arena and the page at
0x90000), once without its magic (the guard off). Every address the program uses is inside the heap's buffer, so
neither run can fault, whatever the guard does: the guard-on run shows that the refused accesses read 0 and left
memory alone although memory was there, and the guard-off run that the same module then reads it.

    python -I scripts/run_address_guard_gpu_test.py --build runtime/build [--llvm-mc CMD] [--work DIR]

Pin the GPU (MESA_VK_DEVICE_SELECT, VK_DRIVER_FILES) as for scripts/run_gpu_tests.py. Exit 0 when both runs give
the expected words, 1 otherwise, 77 when vk_compute_test or llvm-mc is missing.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import address_guard as ag  # noqa: E402

NAME = "cs_address_guard"
HEAP_BYTES = 2 << 20
LIMIT = ag.RESERVED_END          # the "arena": offsets below it pass without a lookup
TABLE = 0x100000                 # the page table, inside the buffer but past the arena
BACKED = [(0x90000, 0x91000)]    # the one page outside the arena the table backs
SEEDS = {0x90000: 0xA11CE000, 0x90004: 0xA11CE004, 0x90FFC: 0xF00DF00D, 0x91000: 0xF00DF00E,
         0xA0000: 0xBADBAD00, 0xA0004: 0xBADBAD04, 0xA0008: 0xBADBAD08}
# out[0..7] (cs_address_guard.asm's header) and other words after the run.
EXPECTED_ON = [0xA11CE000, 0, 0, 0, 0, 0, 0xA11CE004, 0]
EXPECTED_OFF = [0xA11CE000, 0xBADBAD00, 0xBADBAD08, 0xF00DF00D, 0xF00DF00E, 0xBADBAD00, 0xA11CE004, 0xBADBAD00]
MEMORY_ON = {0xA0004: 0xBADBAD04, 0xA0008: 0xBADBAD08,      # untouched
             ag.DUMP + 4: 0x5707, ag.DUMP + 8: 1,           # where the refused store and atomic went
             ag.HEADER + 24: 6,                             # six refused accesses
             ag.HEADER + 32: 0xA0000, ag.HEADER + 36: 0}    # the last one, the s_load at 0xa0000
MEMORY_OFF = {0xA0004: 0x5707, 0xA0008: 0xBADBAD09, ag.DUMP + 4: 0, ag.HEADER + 24: 0}


def heap(guard_on):
    """The heap's bytes: header (without the magic when not `guard_on`), seeds and page table; zero elsewhere."""
    words = ag.table_words(BACKED)
    end = TABLE + 4 * len(words)
    assert end <= HEAP_BYTES
    buf = bytearray(end)
    hdr = bytearray(ag.header(LIMIT, TABLE))
    if not guard_on:
        hdr[0:8] = bytes(8)
    buf[ag.HEADER:ag.HEADER + len(hdr)] = hdr
    for at, value in SEEDS.items():
        struct.pack_into("<I", buf, at, value)
    struct.pack_into(f"<{len(words)}I", buf, TABLE, *words)
    return bytes(buf)


def find_tool(build, name):
    for sub in ("", "RelWithDebInfo", "Release", "Debug"):
        for ext in ("", ".exe"):
            path = os.path.join(build, sub, name + ext)
            if os.path.isfile(path):
                return path
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--build", default=os.path.join(ROOT, "runtime", "build"), help="folder with vk_compute_test")
    # No literal default: unset, mc_command() reads LLVM_MC and then finds llvm-mc on PATH or a versioned
    # Debian/Ubuntu install (llvm-mc-21, /usr/lib/llvm-21/bin/llvm-mc), as run_gpu_tests.py does.
    ap.add_argument("--llvm-mc", default=None)
    ap.add_argument("--work", default=None, help="keep the intermediate files here")
    a = ap.parse_args(argv)
    vk_compute = find_tool(a.build, "vk_compute_test")
    if not vk_compute:
        print(f"SKIP: no vk_compute_test under {a.build}")
        return 77
    import mc_command
    mc = mc_command.mc_command(a.llvm_mc)
    if not shutil.which(mc[0]):
        print(f"SKIP: {' '.join(mc)} not found")
        return 77
    sys.path.insert(0, os.path.join(ROOT, "runtime", "test", "shaders"))
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    from compute_fixture import compute_header
    import run_gpu_tests
    work = a.work or tempfile.mkdtemp(prefix="leonida-guard-")
    asm = os.path.join(ROOT, "runtime", "test", "shaders", NAME + ".asm")
    for sub in ("disasm", "fssm", "spirv"):
        os.makedirs(os.path.join(work, sub), exist_ok=True)
    run_gpu_tests.assemble_to_s(a.llvm_mc, asm, os.path.join(work, "disasm", NAME + ".s"), NAME)
    with open(os.path.join(work, NAME + ".dxbc"), "wb") as f:
        f.write(run_gpu_tests.make_dxbc([]))
    with open(os.path.join(work, "fssm", NAME + ".fssm"), "wb") as f:
        f.write(compute_header(asm))
    spirv = os.path.join(work, "spirv")
    p = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "rdna2spirv.py"),
                        os.path.join(work, "disasm"), "--out", spirv, "--address-guard"],
                       capture_output=True, text=True)
    spv = os.path.join(spirv, NAME + ".spv")
    if p.returncode != 0 or not os.path.isfile(spv):
        print(p.stdout + p.stderr)
        print("FAIL: translation")
        return 1
    failures = 0
    for guard_on, expected, memory in ((True, EXPECTED_ON, MEMORY_ON), (False, EXPECTED_OFF, MEMORY_OFF)):
        label = "guard on" if guard_on else "guard off"
        init, dump = os.path.join(work, f"heap_{label[6:]}.bin"), os.path.join(work, f"dump_{label[6:]}.bin")
        with open(init, "wb") as f:
            f.write(heap(guard_on))
        env = dict(os.environ, VKC_HEAP_INIT=init, VKC_OUT_WORDS=str(HEAP_BYTES // 4), VKC_DUMP=dump)
        r = subprocess.run([vk_compute, spv, "1", "dump"], capture_output=True, text=True, env=env, timeout=120)
        if r.returncode != 0 or not os.path.isfile(dump):
            print(f"FAIL ({label}): vk_compute_test exit {r.returncode}\n{r.stdout}{r.stderr}")
            failures += 1
            continue
        with open(dump, "rb") as f:
            data = f.read()
        word = lambda at: struct.unpack_from("<I", data, at)[0]   # noqa: E731
        got = [word(4 * k) for k in range(8)]
        bad = [f"out[{k}] = {g:#x}, want {w:#x}" for k, (g, w) in enumerate(zip(got, expected)) if g != w]
        bad += [f"[{at:#x}] = {word(at):#x}, want {w:#x}" for at, w in memory.items() if word(at) != w]
        print(f"{'PASS' if not bad else 'FAIL'} ({label}): out = {' '.join(f'{g:#x}' for g in got)}")
        for line in bad:
            print("  " + line)
        failures += bool(bad)
    return 1 if failures else 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
