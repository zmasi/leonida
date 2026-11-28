"""The address guard's page table and header, as the runtime writes them (runtime/src/address_guard.h) and the
shaders translated with `rdna2spirv.py --address-guard` read them (docs/d3d12_vulkan.md, "Address guard").

Standard library only. Used by the translator tests and scripts/run_address_guard_gpu_test.py to build a heap, and
as the Python reference of the check a guarded access makes:

    python -I tools/address_guard.py 0x90000:0x91000 --check 0x90ffc 8

Offsets are heap offsets (device address - heap_base), taken as 48-bit values: a negative one (a buffer below the
arena) is its two's complement in 48 bits, as the shaders sign-extend a V#'s base.
"""
import argparse
import struct
import sys

# Shared with tools/rdna2spirv.py (ADDRESS_GUARD and the GUARD_* constants) and runtime/src/address_guard.h.
HEADER = 0x70000          # arena offset of the header
ZERO = 0x71000            # refused loads read here (+ the offset within its 4 KiB page)
DUMP = 0x78000            # refused stores and atomics write here
RESERVED_END = 0x80000    # the guard's part of the arena ends here
MAGIC = int.from_bytes(b"LGUARD01", "little")
L1 = 1 << 16              # level-1 words: bits 47..32 of the offset
BLOCK_WORDS = 1 << 15     # a level-2 block: one bit per 4 KiB page of a 4 GiB chunk
PAGE = 1 << 12
MASK48 = (1 << 48) - 1
EMPTY_BLOCK, FULL_BLOCK = 0, 1   # every table has them; allocated blocks start at 2


def header(limit, table_offset, refused=0, last=0, last_module=0):
    """The 64-byte header: magic, arena size (offsets below it pass without a lookup), the table's address minus
    heap_base (mod 2^64), the refusal count, the last refusing module id, and the last refused offset, which the
    shaders write."""
    return struct.pack(
        "<QQQIIQ24x",
        MAGIC,
        limit,
        table_offset & ((1 << 64) - 1),
        refused & 0xFFFFFFFF,
        last_module & 0xFFFFFFFF,
        last & ((1 << 64) - 1),
    )


def _mark_pages(chunks, first, last):
    """Set the page bits for inclusive 48-bit page range [first, last] in per-chunk level-2 bitmaps."""
    for chunk in range(first >> 20, (last >> 20) + 1):
        bits = chunks.get(chunk)
        if bits is None:
            bits = [0] * BLOCK_WORDS
            chunks[chunk] = bits
        lo = max(first, chunk << 20) & 0xFFFFF
        hi = min(last, (chunk << 20) | 0xFFFFF) & 0xFFFFF
        w_lo, b_lo = lo >> 5, lo & 31
        w_hi, b_hi = hi >> 5, hi & 31
        if w_lo == w_hi:
            bits[w_lo] |= ((1 << (b_hi - b_lo + 1)) - 1) << b_lo
        else:
            bits[w_lo] |= (0xFFFFFFFF << b_lo) & 0xFFFFFFFF
            for w in range(w_lo + 1, w_hi):
                bits[w] = 0xFFFFFFFF
            bits[w_hi] |= (1 << (b_hi + 1)) - 1


def table_words(spans, max_blocks=None):
    """The table's words for backed [start, end) heap offset spans: level 1, then the level-2 blocks. Spans are
    48-bit heap offsets and may wrap through 2^48 (for a negative offset below the arena, matching
    runtime/src/address_guard.h). A page is backed when any byte of it is (a GPU page is mapped whole). Past
    `max_blocks` allocated blocks a chunk gets the full block: everything in it passes, as the runtime does when
    it runs out of room (and logs it)."""
    chunks = {}
    max_page = MASK48 >> 12
    for start, end in spans:
        if ((end - start) & MASK48) == 0:
            continue
        first = (start & MASK48) >> 12
        last = ((end - 1) & MASK48) >> 12
        if first <= last:
            _mark_pages(chunks, first, last)
        else:  # wraps through 2^48: two runs (runtime/src/address_guard.h)
            _mark_pages(chunks, first, max_page)
            _mark_pages(chunks, 0, last)
    level1 = [EMPTY_BLOCK] * L1
    blocks = []
    for chunk in sorted(chunks):
        if max_blocks is not None and len(blocks) >= max_blocks:
            level1[chunk] = FULL_BLOCK
            continue
        level1[chunk] = 2 + len(blocks)
        blocks.append(chunks[chunk])
    words = level1 + [0] * BLOCK_WORDS + [0xFFFFFFFF] * BLOCK_WORDS
    for bits in blocks:
        words += bits
    return words


def page_backed(words, offset):
    """The table's bit for the page of heap offset `offset`."""
    offset &= MASK48
    block = words[(offset >> 32) & (L1 - 1)]
    page = (offset >> 12) & 0xFFFFF
    return bool(words[L1 + block * BLOCK_WORDS + (page >> 5)] >> (page & 31) & 1)


def passes(words, limit, offset, extent):
    """What a guarded access does: True when [offset, offset + extent) is below `limit` (the arena) or the pages of
    its first and last byte are backed. `offset` is sign-extended from 48 bits, as the shaders do."""
    offset &= MASK48
    if offset >> 47:
        offset -= 1 << 48
    end = offset + extent - 1
    if 0 <= offset < limit and 0 <= end < limit:
        return True
    return page_backed(words, offset) and page_backed(words, end)


def _int(text):
    return int(text, 0)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("spans", nargs="*", help="backed heap offset spans, START:END (hex or decimal)")
    ap.add_argument("--limit", type=_int, default=RESERVED_END, help="the arena's size (default %(default)#x)")
    ap.add_argument("--check", nargs=2, type=_int, metavar=("OFFSET", "BYTES"), action="append", default=[],
                    help="print whether an access passes")
    a = ap.parse_args(argv)
    spans = []
    for text in a.spans:
        start, _, end = text.partition(":")
        spans.append((_int(start), _int(end)))
    words = table_words(spans)
    blocks = (len(words) - L1) // BLOCK_WORDS
    print(f"table: {len(words) * 4} bytes, {blocks - 2} level-2 block(s) besides the empty and full ones")
    for offset, extent in a.check:
        print(f"{offset:#x} +{extent}: {'passes' if passes(words, a.limit, offset, extent) else 'refused'}")
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
