"""Report the statically resolved ID3D12Device vtable call sites of an x86-64 PE image, for the device paths it
follows, from the file alone (RT-566).

Usage:
    python -I tools/d3d12_device_calls.py <image> --out DIR

Writes <image file name>.device_calls.txt (slot numbers and counts only) and .json (the same, plus each site's RVA for
local follow-up) into DIR, and prints the text report. Nothing is run: the image is read as bytes, and its code is
decoded with tools/gdk_runtime_calls.py's x86-64 decoder.

How it follows the device (x86-64 images only):
1. The creators are the imports `D3D12XboxCreateDevice` and `D3D12CreateDevice` (any DLL). A creator is called as
   `call [rip+IAT]`, `call` to an import thunk `jmp [rip+IAT]`, or `call reg` after `mov reg,[rip+IAT]`. Both take
   the device's out-pointer as their fourth argument (r9).
2. The out-pointer, followed through `lea` and register copies before the call: `lea r9,[rip+G]` makes G a device
   global; `lea r9,[reg+disp]` makes that memory slot hold the device after the call. Any other r9 (a pointer the
   function was given, a tail jump to the creator) is reported as an out-pointer not followed.
3. Within each function (bounds from .pdata; a byte window for code without an entry) the device is followed through
   loads of a device global, register copies, stores to and loads from the same `[reg+disp]` slot (stack slots
   survive calls; other memory doesn't), and stores to another global, which becomes a device global too. Every
   function that references a device global is walked again until no new global turns up.
   The walk is linear, with forward branches followed: a `jmp` or conditional branch to a later address in the
   walked range carries what is known at the branch to its target, where it is met with the fall-through's state
   (a fact is kept when both paths agree on it) or, past a `ret` or unconditional jump, replaces it. Only a `jmp`
   out of the walked range is a tail call; one back into it (a loop) carries nothing.
4. A device call is `mov v,[dev]` (the vtable) then `call [v+disp]`, or `mov m,[v+disp]` then `call m`, or a direct
   call to a dispatch routine with the method in rax (the CFG form). The routine must be verified: the target (for
   `call [rip+p]`, the code the file's pointer p holds) reaches `jmp rax` or `call rax` within DISPATCH_WINDOW
   instructions without writing rax, calling or leaving first (conditional branches are allowed, not followed), as
   `_guard_dispatch_icall_nop` (`jmp rax`) and a guard check that ends in `jmp rax` do. A call to any other target
   with a method in rax is reported as not resolved, never as a slot. The slot is disp / 8. A `jmp`
   in place of the call is a tail jump. Which register holds the device at the call is recorded: rcx is the usual
   `this`; rdx is the console's return-buffer form, where rcx holds the buffer (docs/retail_boot_chain.md §3.1).
   The argument count, `this` included, is an estimate: the highest of rcx/rdx/r8/r9 (xmm1-3) written since the
   previous call, or the stack arguments [rsp+20h+8k] stored since then from k = 0 up without a gap.

What it reports as not resolved, never guessing a slot: a vtable load from the device not followed by a call the
patterns above match (an indexed or unaligned method load, a method pointer compared or stored, a path the walk
loses, such as a loop's back edge), a direct call with a method in
rax whose target isn't a verified dispatch routine, a call with the
device in an argument register that isn't a vtable call (the device handed to another function, which is not
followed into), the device stored to non-stack memory (followed only within the function), and creator calls whose
out-pointer isn't followed. The report holds the statically resolved sites for the device paths it follows; a
device reached only through those paths (a member of an object, a function parameter, a getter's return value) is
not followed, so a slot can be called more often than the report says, or at all when the report has no row for it.

Slot names: slots 0-43 carry the PC ID3D12Device method names (public d3d12.h order). The console keeps that order
where it was measured (slots 8, 9, 14, 26, 36: docs/retail_boot_chain.md, runtime/shims/d3d12_xs_device.h); the
other names in that range are the PC ones, not measured on the console. Past 43 the console layout differs from the
PC one, so only the slots this repository has measured or takes from a public HLE are named (the same files); the
others are left unnamed.

Exit status 1 when the image can't be read or isn't an x86-64 PE32+ image, or the report can't be written;
0 otherwise (also when the image imports no creator: the report says so). Standard library only; no game data.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
from file_mode import published_mode  # noqa: E402
from gdk_runtime_calls import (  # noqa: E402
    ARG_INDEX, RAX, RCX, RDX, RIP, RSP, R9, VOLATILE, Code, branches, stack_argument, writes)
from import_call_sites import import_slots  # noqa: E402
from pe_inspect import PeFormatError, PeImage  # noqa: E402
import replace_retry  # noqa: E402
from report_paths import same_report_path  # noqa: E402

RBP = 5
CREATORS = ("D3D12XboxCreateDevice", "D3D12CreateDevice")
NO_PDATA_WINDOW = 512       # bytes walked from a reference that no .pdata entry covers
DISPATCH_WINDOW = 16        # instructions a dispatch routine may take before its `jmp rax` / `call rax`
MAX_ROUNDS = 16             # rounds of "walk every function that references a device global"

PC_DEVICE = (
    "QueryInterface", "AddRef", "Release", "GetPrivateData", "SetPrivateData", "SetPrivateDataInterface", "SetName",
    "GetNodeCount", "CreateCommandQueue", "CreateCommandAllocator", "CreateGraphicsPipelineState",
    "CreateComputePipelineState", "CreateCommandList", "CheckFeatureSupport", "CreateDescriptorHeap",
    "GetDescriptorHandleIncrementSize", "CreateRootSignature", "CreateConstantBufferView",
    "CreateShaderResourceView", "CreateUnorderedAccessView", "CreateRenderTargetView", "CreateDepthStencilView",
    "CreateSampler", "CopyDescriptors", "CopyDescriptorsSimple", "GetResourceAllocationInfo",
    "GetCustomHeapProperties", "CreateCommittedResource", "CreateHeap", "CreatePlacedResource",
    "CreateReservedResource", "CreateSharedHandle", "OpenSharedHandle", "OpenSharedHandleByName", "MakeResident",
    "Evict", "CreateFence", "GetDeviceRemovedReason", "GetCopyableFootprints", "CreateQueryHeap",
    "SetStablePowerState", "CreateCommandSignature", "GetResourceTiling", "GetAdapterLuid")
CONSOLE_MEASURED = frozenset((8, 9, 14, 26, 36))
SHIM = "runtime/shims/d3d12_xs_device.h"
CONSOLE_NAMES = {
    48: ("CreateCommandQueueX", f"public HLE, {SHIM}"),
    62: ("CreateComputePipelineState (console form)", f"measured, {SHIM}"),
    67: ("CreateComputePipelineState from native code", f"measured, {SHIM}"),
    73: ("CreateCommandListX", f"public HLE, {SHIM}"),
    76: ("resource creation (CreateCommittedResource form)", f"measured, {SHIM}"),
    78: ("SetFrameIntervalX", f"public HLE, {SHIM}"),
    79: ("ScheduleFrameEventX", f"public HLE, {SHIM}"),
    80: ("WaitFrameEventX", f"public HLE, {SHIM}"),
    133: ("CreatePlacedResourceX (shape)", "measured, docs/retail_boot_chain.md §3.4"),
}


def slot_name(slot: int) -> tuple[str | None, str | None]:
    """(name, where the name comes from) for a device slot, or (None, None)."""
    if slot < len(PC_DEVICE):
        if slot in CONSOLE_MEASURED:
            return PC_DEVICE[slot], "PC order, console measured"
        return PC_DEVICE[slot], "PC order"
    return CONSOLE_NAMES.get(slot, (None, None))


# `mov`/`lea`/`call`/`jmp` with a RIP-relative operand and no immediate: [REX] 8B|89|8D|FF modrm(00 reg 101) disp32.
RIP_OPERAND = re.compile(rb"[\x40-\x4f]?[\x89\x8b\x8d\xff][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.S)


def rip_references(code: Code, targets: set[int]) -> list[int]:
    """Start RVAs of the instructions (by byte pattern, checked later by decoding) whose RIP-relative operand is one
    of `targets`."""
    out = []
    unpack = struct.Struct("<i").unpack_from
    for base, data in code.sections:
        size = len(data)
        for m in RIP_OPERAND.finditer(data):
            end = m.end()
            if end + 4 > size:
                break
            if base + end + 4 + unpack(data, end)[0] in targets:
                out.append(base + m.start())
    return out


def argument_count(written: set[int], stack: set[int]) -> int:
    """Estimated argument count, `this` included: the highest argument register written, then the stack arguments
    stored from the fifth on without a gap (a store further up the frame is more likely a local)."""
    n = max([ARG_INDEX[r] for r in written if r in ARG_INDEX] + [0])
    if 4 in stack:
        n = 4
        while n + 1 in stack:
            n += 1
    return n + 1


class Census:
    """The device's call sites in one image."""

    def __init__(self, img: PeImage):
        self.img = img
        self.code = Code(img)
        slots = import_slots(img)
        self.creators = {rva: name for rva, name in slots.items() if name.rpartition("!")[2] in CREATORS}
        self.thunks: dict[int, str] = {}
        for source, target in branches(self.code, b"\xff\x25"):
            if target in self.creators:
                i = self.code.decode(source)
                if i is not None and i.op == 0xFF and i.base == RIP and i.target == target:
                    self.thunks[source] = self.creators[target]
        self.holders: set[int] = set()
        self.dispatchers: dict[int, bool] = {}      # code rva -> verified dispatch routine
        self.reset()

    def code_pointer(self, rva: int) -> int | None:
        """The code RVA the 8-byte pointer at `rva` holds in the file, or None."""
        at = self.img.offset(rva, 8)
        if at is None:
            return None
        target = self.img.word(at) - self.img.image_base
        return target if self.code.view(target)[0] is not None else None

    def is_dispatcher(self, rva: int | None) -> bool:
        """Whether the code at `rva` reaches `jmp rax` or `call rax` within DISPATCH_WINDOW instructions without
        writing rax, calling, returning or jumping elsewhere first."""
        if rva is None:
            return False
        if rva not in self.dispatchers:
            ok, at = False, rva
            for _ in range(DISPATCH_WINDOW):
                i = self.code.decode(at)
                if i is None:
                    break
                if i.op == 0xFF and i.mod == 3 and i.rm == RAX and i.reg & 7 in (2, 4):
                    ok = True
                    break
                if i.is_call() or i.ends_flow() or i.op in (0xC3, 0xC2, 0xCC) or RAX in writes(i):
                    break
                at = i.end
            self.dispatchers[rva] = ok
        return self.dispatchers[rva]

    def reset(self) -> None:
        self.sites: dict[int, dict] = {}            # call rva -> site
        self.creator_calls: dict[int, dict] = {}    # call rva -> {"import", "out"}
        self.vtable_loads: dict[int, bool] = {}     # load rva -> a call used it
        self.passed: set[int] = set()               # calls that got the device in an argument register
        self.unverified: set[int] = set()           # direct calls with a method in rax to an unverified target
        self.stored: set[int] = set()               # stores of the device to non-stack memory
        self.new_holders: set[int] = set()
        self.undecodable = 0
        self.walked = 0

    # ---- walking one stretch of code ----

    def walk(self, start: int, stop: int, window: bool) -> None:
        dev: set[int] = set()                       # registers holding the device
        vt: dict[int, frozenset] = {}               # register -> rvas of the vtable loads it may hold
        meth: dict[int, tuple[int, frozenset]] = {}  # register -> (slot, vtable load rvas) of the method it holds
        addr: dict[int, tuple] = {}                 # register -> ("g", G) or ("m", base, disp): an address it holds
        creator_reg: dict[int, str] = {}            # register -> creator import loaded from its IAT slot
        mem: set[tuple[int, int]] = set()           # [base+disp] slots holding the device
        written: set[int] = set()
        stack: set[int] = set()
        frame_rbp = False
        pending: dict[int, tuple] = {}              # forward branch target -> the state the branches carry there
        dead = False                                # past an unconditional jump, ret or tail call, before the
                                                    # next forward-branch target: no known path reaches here

        def clear() -> None:
            dev.clear(), vt.clear(), meth.clear(), addr.clear(), creator_reg.clear(), mem.clear()

        def forget(r: int) -> None:
            dev.discard(r)
            vt.pop(r, None)
            meth.pop(r, None)
            addr.pop(r, None)
            creator_reg.pop(r, None)
            for key in [k for k in mem if k[0] == r]:
                mem.discard(key)

        def survives_call(base: int) -> bool:
            return base == RSP or (base == RBP and frame_rbp)

        def snapshot() -> tuple:
            return (set(dev), dict(vt), dict(meth), dict(addr), dict(creator_reg), set(mem), set(written),
                    set(stack))

        def restore(state: tuple) -> None:
            for mine, theirs in zip((dev, vt, meth, addr, creator_reg, mem, written, stack), state):
                mine.clear()
                mine.update(theirs)

        def meet(a: tuple, b: tuple) -> tuple:
            """What holds on both paths into a join: a register keeps a fact only when both paths agree on it
            (vtable loads are merged, since either path's load is the device's vtable)."""
            same = lambda x, y: {r: x[r] for r in x.keys() & y.keys() if x[r] == y[r]}  # noqa: E731
            return (a[0] & b[0],
                    {r: a[1][r] | b[1][r] for r in a[1].keys() & b[1].keys()},
                    {r: (a[2][r][0], a[2][r][1] | b[2][r][1]) for r in a[2].keys() & b[2].keys()
                     if a[2][r][0] == b[2][r][0]},
                    same(a[3], b[3]), same(a[4], b[4]), a[5] & b[5], a[6] | b[6], a[7] | b[7])

        def carry(target: int) -> None:
            state = snapshot()
            pending[target] = meet(pending[target], state) if target in pending else state

        def mark_used(loads) -> None:
            for load in loads:
                self.vtable_loads[load] = True

        self.walked += 1
        rva = start
        while rva < stop:
            if rva in pending:                      # a forward branch's target: the branches' state, met with
                carried_in = pending.pop(rva)       # the fall-through's when the code before it is reached
                restore(carried_in if dead else meet(snapshot(), carried_in))
                dead = False
            i = self.code.decode(rva)
            if i is None:
                self.undecodable += 1
                return
            rva = i.end
            op, mod, base = i.op, i.mod, i.base
            if op in (0xE9, 0xEB) and start <= i.target < stop and i.target not in self.thunks:
                # An unconditional jump within the walked range is a branch, not a tail call: a forward one
                # carries its state to its target; a backward one (a loop) is not followed. Either way the
                # next instruction is not reached from here.
                if i.target > i.rva and not dead:
                    carry(i.target)
                clear()
                written.clear()
                stack.clear()
                dead = True
                continue
            if (0x70 <= op <= 0x7F or 0x180 <= op <= 0x18F) and start <= i.target < stop and i.target > i.rva \
                    and not dead:
                carry(i.target)                     # a conditional branch: both paths go on
            indirect = op == 0xFF and mod is not None and i.reg & 7 in (2, 4)
            if op in (0xE8, 0xE9) or indirect:
                tail = op == 0xE9 or (indirect and i.reg & 7 == 4)
                slot = None
                vload = via = None
                if indirect and mod != 3 and base in vt:
                    if i.index is None and i.disp >= 0 and i.disp % 8 == 0:
                        slot, vload, via = i.disp // 8, vt[base], "vtable"  # call [vtable+disp]
                elif indirect and mod == 3 and i.rm in meth:
                    slot, vload, via = *meth[i.rm], "register"         # call reg
                elif (op in (0xE8, 0xE9) or (indirect and base == RIP)) and RAX in meth:
                    target = i.target if op in (0xE8, 0xE9) else self.code_pointer(i.target)
                    if self.is_dispatcher(target):
                        slot, vload, via = *meth[RAX], "dispatch"      # dispatch routine, method in rax
                    elif not (i.target in self.thunks or (indirect and i.target in self.creators)):
                        self.unverified.add(i.rva)
                        mark_used(meth[RAX][1])                        # reported here, not as an unused load
                creator = None
                if indirect and base == RIP and i.target in self.creators:
                    creator = self.creators[i.target]
                elif op in (0xE8, 0xE9) and i.target in self.thunks:
                    creator = self.thunks[i.target]
                elif indirect and mod == 3 and i.rm in creator_reg:
                    creator = creator_reg[i.rm]
                out = addr.get(R9)
                if creator is not None:
                    slot = None
                    record = {"import": creator, "out": "not followed"}
                    if tail:
                        record["out"] = "not followed (tail jump)"
                    elif out and out[0] == "g":
                        record["out"] = "global"
                        self.new_holders.add(out[1])
                    elif out and out[0] == "m" and survives_call(out[1]):
                        record["out"] = "stack slot"
                    elif out and out[0] == "m" and out[1] not in VOLATILE:
                        record["out"] = "object member (followed until the next call)"
                    self.creator_calls[i.rva] = record
                if slot is not None:
                    this = "rcx" if RCX in dev else "rdx" if RDX in dev else "other"
                    args = argument_count(written, stack)
                    self.sites[i.rva] = {"rva": i.rva, "slot": slot, "kind": "jmp" if tail else "call",
                                         "via": via, "this": this, "args": args}
                    mark_used(vload)
                elif (creator is None and not (indirect and mod != 3 and base in vt) and i.rva not in self.unverified
                      and dev & {RCX, RDX, 8, 9}):
                    self.passed.add(i.rva)
                for r in VOLATILE:
                    forget(r)
                for key in [k for k in mem if not survives_call(k[0])]:
                    mem.discard(key)
                if creator is not None and not tail and out and out[0] == "m" and (
                        survives_call(out[1]) or out[1] not in VOLATILE):
                    mem.add((out[1], out[2]))
                written.clear()
                stack.clear()
                if tail:
                    clear()
                    dead = True
                    if window and not pending:
                        return
                continue
            if i.ends_flow():
                clear()
                dead = True
                if window and not pending:
                    return
                continue

            k = stack_argument(i)
            if k is not None:
                stack.add(k)
            wide = bool(i.rex & 8)
            plain = mod is not None and mod != 3 and i.index is None and base is not None
            new_dev = new_vt = new_meth = new_addr = new_creator = None
            copy_from = None
            if op == 0x8B and wide and plain:                          # mov r64, [m]
                if base == RIP:
                    if i.target in self.holders or i.target in self.new_holders:
                        new_dev = True
                    elif i.target in self.creators:
                        new_creator = self.creators[i.target]
                elif base in dev and i.disp == 0:
                    new_vt = frozenset((i.rva,))
                elif base in vt and i.disp >= 0 and i.disp % 8 == 0:
                    new_meth = (i.disp // 8, vt[base])
                elif (base, i.disp) in mem:
                    new_dev = True
            elif op == 0x8D and wide and plain:                        # lea r64, [m]
                new_addr = ("g", i.target) if base == RIP else ("m", base, i.disp)
            elif wide and mod == 3 and op in (0x8B, 0x89):             # mov r64, r64
                copy_from = i.rm if op == 0x8B else i.reg
            if op in (0x88, 0x89, 0xC6, 0xC7, 0x87, 0x86) and mod is not None and mod != 3:   # a store to [m]
                if base == RIP:
                    if op == 0x89 and wide and i.reg in dev:
                        self.new_holders.add(i.target)
                elif plain:
                    key = (base, i.disp)
                    if op == 0x89 and wide and i.reg in dev:
                        mem.add(key)
                        if base not in (RSP, RBP):
                            self.stored.add(i.rva)
                    else:
                        mem.discard(key)
            dests = writes(i)
            if copy_from is not None:
                carried = (copy_from in dev, vt.get(copy_from), meth.get(copy_from), addr.get(copy_from),
                           creator_reg.get(copy_from))
            if RBP in dests:
                frame_rbp = ((op == 0x8D and base == RSP) or (copy_from == RSP))
            for r in dests:
                forget(r)
            written.update(dests)
            if not dests:
                continue
            dst = dests[0]
            if copy_from is not None:
                if carried[0]:
                    dev.add(dst)
                if carried[1] is not None:
                    vt[dst] = carried[1]
                if carried[2] is not None:
                    meth[dst] = carried[2]
                if carried[3] is not None:
                    addr[dst] = carried[3]
                if carried[4] is not None:
                    creator_reg[dst] = carried[4]
            elif new_dev:
                dev.add(dst)
            elif new_vt is not None:
                vt[dst] = new_vt
                self.vtable_loads.setdefault(i.rva, False)
            elif new_meth is not None:
                meth[dst] = new_meth
            elif new_addr is not None:
                addr[dst] = new_addr
            elif new_creator is not None:
                creator_reg[dst] = new_creator

    # ---- the whole image ----

    def stretches(self) -> list[tuple[int, int, bool]]:
        """(start, stop, window) of the code to walk: the functions referencing a creator or a device global."""
        refs = rip_references(self.code, set(self.creators) | self.holders)
        if self.thunks:
            refs += [s for s, t in branches(self.code, b"\xe8") if t in self.thunks]
            refs += [s for s, t in branches(self.code, b"\xe9") if t in self.thunks]
        out = set()
        for ref in refs:
            if ref in self.thunks:
                continue
            func = self.code.function(ref)
            if func:
                out.add((func[0], func[1], False))
            else:
                data, sbase = self.code.view(ref)
                out.add((ref, min(ref + NO_PDATA_WINDOW, sbase + len(data)), True))
        return sorted(out)

    def run(self) -> None:
        for _ in range(MAX_ROUNDS):
            self.reset()
            for start, stop, window in self.stretches():
                self.walk(start, stop, window)
            grown = self.new_holders - self.holders
            if not grown:
                return
            self.holders |= grown
        self.rounds_exhausted = True


def scan(data: bytes) -> dict:
    """The census of one image as a dict (see report())."""
    img = PeImage(data)
    if not img.is64 or img.machine != 0x8664:
        raise PeFormatError("only x86-64 PE32+ images are scanned")
    c = Census(img)
    c.run()
    slots: dict[int, dict] = {}
    for site in c.sites.values():
        s = slots.setdefault(site["slot"], {"slot": site["slot"], "sites": 0, "calls": 0, "tail_jumps": 0,
                                            "this_rcx": 0, "this_rdx": 0, "this_other": 0, "args": []})
        s["sites"] += 1
        s["calls" if site["kind"] == "call" else "tail_jumps"] += 1
        s["this_" + site["this"]] += 1
        s["args"].append(site["args"])
    table = []
    for slot in sorted(slots):
        s = slots[slot]
        name, source = slot_name(slot)
        args = s.pop("args")
        table.append({**s, "name": name, "name_source": source, "args_min": min(args), "args_max": max(args)})
    creators: dict[str, dict] = {}
    for rec in c.creator_calls.values():
        e = creators.setdefault(rec["import"], {"import": rec["import"], "call_sites": 0, "out": {}})
        e["call_sites"] += 1
        e["out"][rec["out"]] = e["out"].get(rec["out"], 0) + 1
    for name in sorted(set(c.creators.values())):
        creators.setdefault(name, {"import": name, "call_sites": 0, "out": {}})
    unused = sum(1 for used in c.vtable_loads.values() if not used)
    return {
        "creators": [creators[k] for k in sorted(creators)],
        "device_globals": len(c.holders),
        "functions_walked": c.walked,
        "undecodable_walks": c.undecodable,
        "rounds_exhausted": getattr(c, "rounds_exhausted", False),
        "sites": len(c.sites),
        "calls": sum(1 for s in c.sites.values() if s["kind"] == "call"),
        "tail_jumps": sum(1 for s in c.sites.values() if s["kind"] == "jmp"),
        "dispatch_sites": sum(1 for s in c.sites.values() if s["via"] == "dispatch"),
        "slots": table,
        "unresolved": {
            "vtable_loads_without_matched_call": unused,
            "dispatch_calls_to_unverified_targets": len(c.unverified),
            "device_passed_to_other_calls": len(c.passed),
            "device_stored_to_non_stack_memory": len(c.stored),
            "creator_calls_out_not_followed": sum(1 for r in c.creator_calls.values()
                                                  if r["out"].startswith("not followed")),
        },
        "site_rvas": [{"rva": f"0x{s['rva']:x}", "slot": s["slot"], "kind": s["kind"], "this": s["this"]}
                      for s in sorted(c.sites.values(), key=lambda s: s["rva"])],
    }


def to_text(file: str, r: dict) -> str:
    lines = [f"{file}: ID3D12Device vtable calls, from the file alone (tools/d3d12_device_calls.py)", ""]
    if not r["creators"]:
        lines.append("No D3D12XboxCreateDevice or D3D12CreateDevice import: nothing to follow.")
    for c in r["creators"]:
        outs = ", ".join(f"{k} {v}" for k, v in sorted(c["out"].items())) or "-"
        lines.append(f"creator {c['import']}: {c['call_sites']} call site(s); out-pointer: {outs}")
    lines.append(f"device globals: {r['device_globals']}; functions walked: {r['functions_walked']}"
                 + (f"; walks stopped at undecodable bytes: {r['undecodable_walks']}" if r["undecodable_walks"] else ""))
    if r["rounds_exhausted"]:
        lines.append(f"warning: the device globals were still growing after {MAX_ROUNDS} rounds")
    lines.append("")
    lines.append(f"{'slot':>4}  {'sites':>5}  {'call':>4}  {'jmp':>3}  {'rcx':>3}  {'rdx':>3}  {'oth':>3}  "
                 f"{'args':>5}  name (source)")
    for s in r["slots"]:
        args = str(s["args_min"]) if s["args_min"] == s["args_max"] else f"{s['args_min']}-{s['args_max']}"
        name = f"{s['name']} ({s['name_source']})" if s["name"] else "-"
        lines.append(f"{s['slot']:>4}  {s['sites']:>5}  {s['calls']:>4}  {s['tail_jumps']:>3}  {s['this_rcx']:>3}  "
                     f"{s['this_rdx']:>3}  {s['this_other']:>3}  {args:>5}  {name}")
    lines.append("")
    lines.append(f"{r['sites']} site(s) across {len(r['slots'])} slot(s): {r['calls']} indirect vtable call(s) and "
                 f"{r['tail_jumps']} indirect vtable tail jump(s), {r['dispatch_sites']} of them through a verified "
                 f"dispatch routine")
    u = r["unresolved"]
    lines.append("not resolved (no slot guessed):")
    lines.append(f"  device vtable loads with no matched call: {u['vtable_loads_without_matched_call']}")
    lines.append(f"  direct calls with a method in rax to a target not verified as a dispatch routine: "
                 f"{u['dispatch_calls_to_unverified_targets']}")
    lines.append(f"  calls given the device in an argument register (not followed into): "
                 f"{u['device_passed_to_other_calls']}")
    lines.append(f"  stores of the device to non-stack memory (followed in the function only): "
                 f"{u['device_stored_to_non_stack_memory']}")
    lines.append(f"  creator calls whose out-pointer is not followed: {u['creator_calls_out_not_followed']}")
    return "\n".join(lines) + "\n"


def _check_out_dir_ancestors(out_dir: str) -> None:
    """Fail before scanning when --out or any existing ancestor is not a directory."""
    if not out_dir:
        raise OSError("empty --out path")
    cur = os.path.abspath(out_dir)
    while True:
        if os.path.exists(cur) or os.path.islink(cur):
            if not os.path.isdir(cur):
                raise OSError(f"{out_dir} is not a directory")
            return
        parent = os.path.dirname(cur)
        if parent == cur:
            return
        cur = parent


def _write_reports(out_dir: str, reports: tuple[tuple[str, str], ...]) -> None:
    """Ensure `out_dir` exists, stage all reports as UTF-8 LF temp files, then replace atomically."""
    os.makedirs(out_dir, exist_ok=True)
    if not os.path.isdir(out_dir):
        raise OSError(f"{out_dir} is not a directory")
    staged: list[tuple[str, str]] = []
    try:
        for dest, payload in reports:
            if os.path.exists(dest) and os.path.isdir(dest):
                raise OSError(f"{dest} is a directory")
            with tempfile.NamedTemporaryFile(
                "w",
                dir=out_dir,
                prefix="." + os.path.basename(dest) + ".",
                suffix=".tmp",
                encoding="utf-8",
                newline="\n",
                delete=False,
            ) as f:
                tmp_path = f.name
                staged.append((tmp_path, dest))
                f.write(payload)
                f.flush()
            os.chmod(tmp_path, published_mode(dest))
        while staged:
            tmp_path, dest = staged[0]
            replace_retry.replace(tmp_path, dest)
            staged.pop(0)
    finally:
        for tmp_path, _ in staged:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    ap.add_argument("image")
    ap.add_argument("--out", required=True,
                    help="folder for <image>.device_calls.txt and .json (created if missing)")
    a = ap.parse_args(argv)
    name = os.path.basename(a.image)
    stem = os.path.join(a.out, name + ".device_calls")
    report_files = (stem + ".json", stem + ".txt")
    try:
        _check_out_dir_ancestors(a.out)
        for path in report_files:
            if same_report_path(a.image, path):
                raise OSError(f"{path} would overwrite the input")
            if os.path.exists(path) and os.path.isdir(path):
                raise OSError(f"{path} is a directory")
    except OSError as e:
        print(f"cannot write the report: {e.strerror or e}", file=sys.stderr)
        return 1
    try:
        with open(a.image, "rb") as f:
            data = f.read()
        r = scan(data)
    except (OSError, PeFormatError) as e:
        print(f"{a.image}: not a readable x86-64 PE image ({e})", file=sys.stderr)
        return 1
    json_text = json.dumps({"file": name, **r}, indent=1) + "\n"
    text = to_text(name, r)
    try:
        _write_reports(a.out, ((report_files[0], json_text), (report_files[1], text)))
    except OSError as e:
        print(f"cannot write the report: {e.strerror or e}", file=sys.stderr)
        return 1
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
