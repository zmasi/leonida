# The pipeline library in `shaders.rpf`

**Status: measured** on the 1.0.2.1 package. `shaders.rpf` holds one more file behind its 26,503 shader entries:
a library of precompiled pipelines, with 25,471 graphics and 2,693 compute pipeline records and the 21,522 native
RDNA2 programs they run. `tools/shader_carve.py` reads only the raw-deflate shader entries, so until now nothing
read this file. No program, blob or record is reproduced here; the tables hold counts, sizes and offsets.

## 1. Where it is

The archive's data area has two parts. The first 274 MB are the shader entries `shader_carve.py` reads (name,
kind, sizes, raw deflate). The rest, from file offset `0x10579000`, is 4,205 zlib streams: 4,204 inflate to
65,536 bytes and the last to 27,436. Concatenated, they form one 275,540,780-byte file. That is the 33.4%
stream coverage [world_census.md](world_census.md) measured for `shaders.rpf`. The gaps between the streams are 0
to 12 bytes.

`tools/shader_library.py` finds the first stream that inflates to the header below and appends streams until it
has the size the header gives. It fails if the sizes don't match exactly.

**No shaders elsewhere.** The other 74 RPF9 and TTPF archives of the package (214 GB: the root archives except
`movies.rpf`, and everything under `pack/` and `data/`) were scanned raw and through every zlib stream (about 420 GB
inflated) for `DXBC`, `XBCH`, `FSSM`, `FSPX` and the shader entries' `_Wrapped` names. There are no shader entries,
and none of the 148 `XBCH`/`FSSM` hits is a record: no `XBCH` inflates to an `FSSM` blob, and no `FSSM` has a code
offset with an `s_endpgm` after it. The same checks accept the 52 records of `XboxBvhBuilder.dll` and 200 of 200
carved FSSM blobs. No `DXBC` hit has a valid header. So the game's shader programs are the shader entries, this
library and the two DLLs' containers ([embedded_shaders.md](embedded_shaders.md)), as far as raw and zlib data go.

## 2. Layout

All values little-endian.

| offset | size | field |
|---|---:|---|
| 0x00 | 4 | `'P' 'S' 00 09` |
| 0x04 | 32 | name, NUL-padded: `0x65F41EDD` |
| 0x24 | 36 | version string, NUL-padded: `930.048.020` |
| 0x48 | 8 | hash |
| 0x50 | 4 × 4 | graphics pipelines G = 25,471, compute pipelines C = 2,693, blobs B = 54,576, programs P = 21,522 |
| 0x60 | 4 | blob section size, 31,120,020 |
| 0x64 | 4 | code section size, 240,894,464 |
| 0x80 | G × 0x70 | graphics pipeline records |
| | C × 24 | compute pipeline records |
| | B × 8 | blob table: (offset, size) into the blob section |
| | P × 8 | program table: (offset, size) into the code section; every offset is 256-byte aligned |
| | | blob section, then code section |

The sections add up to the file size exactly: 0x80 + 25,471 × 0x70 + 2,693 × 24 + 54,576 × 8 + 21,522 × 8 +
31,120,020 + 240,894,464 = 275,540,780. Every blob and program entry lies inside its section. The blob entries
are contiguous; the program entries are in increasing offset order.

**Graphics record** (0x70 bytes): u64 hash, u32 state blob, u32 pipeline blob, 40 bytes not decoded, then seven
(u32 program, u32 stage blob) pairs. 0xFFFFFFFF means no program. The slot gives the stage. Slot 0 is the pixel
shader, slot 1 the vertex shader and slot 2 the mesh shader. The source containers named in §3 have those stages
in every use §3 links: 24,090 of the 24,111 slot-0 uses, 13,388 of 13,409 slot-1 and all 12,062 slot-2. No linked
use names a source of another stage. Slot 6 is used by 2 records with one program and no stage blob; it is not
identified. Slots 3 to 5 are never used.

| stages | pipelines |
|---|---:|
| VS + PS | 13,383 |
| MS + PS | 10,726 |
| MS only | 1,336 |
| VS only | 24 |
| VS + PS + slot 6 | 2 |

The state blob is 388 to 472 bytes (5,680 distinct). The pipeline blob holds the input layout, with element
names such as `POSITION`, `COLOR`, `TEXCOORD`, `NORMAL`, `TANGENT`, `BLENDWEIGHT` and `BLENDINDICES` for vertex
pipelines (80 bytes, with no elements, for mesh pipelines).

**Compute record** (24 bytes): u64 hash, u32 state blob, u32 blob, u32 program, u32 layout blob. The four parts
have the sizes and shapes of the four [FSPX](fspx.md) kinds:

| record field | size | FSPX kind |
|---|---|---|
| state blob | 28 bytes in all 2,693 | kind 0, dispatch state: thread-group size in words 3-4, wave32 flag (word 4 bit 20), RSRC1, RSRC2 |
| blob | 20 bytes in all 2,693 | kind 1 |
| program | | kind 6 |
| layout blob | 56 to 10,552 bytes | kind 7 |

1,529 of the 2,684 compute programs set the wave32 flag. 90 set RSRC2 `TG_SIZE_EN` (bit 10), which none of the
2,689 carved compute shaders do.

**Programs.** 52 to 239,464 bytes (median 3,968; 238,829,764 in total), padded to 256 bytes with `s_code_end`.
21,463 of them start with the 36-byte prologue FSPX programs have, three 12-byte VOP3 opcode-0x180 words with
literals (`fspx_unpack.code_range` finds it); 59 have none. The code runs from there to the last 4-byte-aligned
`s_endpgm`, as for FSSM blobs. 579 of the first 3,000 pixel programs have more than one. Ending at the first one,
as for FSPX, cut off code their branches reach: 2,281 "branch target outside code" and 1,662 "branch into data" over
all stages.
Each program belongs to exactly one stage: 9,996 PS, 7,498 MS, 2,684 CS, 1,343 VS and the one slot-6 program.

## 3. Where the programs come from

A graphics stage blob and a compute layout blob share one layout. Word 0 is the kind (2 for a graphics stage, 3
for compute). Then comes a head of 16 bytes (kind 2) or 24 bytes (kind 3), whose last word is tagged `0x40`. Then
come 32-byte records tagged `0x41` or `0x42`, then a word `0x43`, a 16-byte digest and 12 zero bytes. All 20,318
distinct stage and layout blobs follow this. In FSPX, the 16 bytes after kind 7's `0x43` word are the container's
`HASH` digest ([fspx.md](fspx.md)). Here they are too: **21,509 of the 21,522 programs** have the `HASH` digest of
one of the carved containers, and every linked program has its container's stage (2,680 CS, 9,991 PS, 7,498 MS,
1,340 VS; the carve index's stage comes from the container's `DXIL` program header). They come from 6,522
distinct carved shaders, and a source has 1 to 257 programs.

None of the programs is byte-identical to its source's FSSM code. Only 47 distinct code bodies, from
`s_inst_prefetch` to the first `s_endpgm`, occur in both. So the library holds **pipeline builds**: the same
shaders compiled again with their pipeline's state. The 13 programs whose digest names no carved container (4 CS,
5 PS, 3 VS, the slot-6 program) are the only ones with no source in the shader entries.

For a VS or PS program without native wave-size metadata, a 32-bit EXEC operation and an agreeing wave32
flag in every FSSM payload of its matching DXBC source distinguish a wave32 program that uses `s_bfm_b64`
as scalar data. The translator keeps its existing behavior when the source is absent, the payloads disagree,
or the library code has another strict wave64 marker. Synthetic cases are in
`tests/semantic/test_fspx_wave_size.py`.

### Containers and programs

The carved corpus is counted two ways, and the two numbers are both right (BL-5 in
[boot_leads.md](boot_leads.md)):

| Count | What it counts | 1.0.2.1 |
|---|---|---|
| Containers | carved `DXBC` files, one row of the carve index each | 26,503 |
| Programs | tag-`0x0D` payloads (`XBCH` → `FSSM`) inside them | 26,698 |

The difference is 195 containers that hold two payloads. All 195 are vertex shaders; every other container holds
one:

| Stage | Containers | Payloads | Containers with two |
|---|---|---|---|
| `vs` | 2,236 | 2,431 | 195 |
| `ms` | 9,282 | 9,282 | 0 |
| `ps` | 12,101 | 12,101 | 0 |
| `gs` | 195 | 195 | 0 |
| `cs` | 2,689 | 2,689 | 0 |

So 195 is the geometry-shader count because of what the second payload is, not because geometry shaders are
counted twice or missed: it is the same vertex shader built for the export-shader stage of a legacy
geometry-shader pipeline ([design.md](design.md), "VS as ES (two-payload VS containers)"). The carve skips
nothing here.

`tools/fssm_unpack.py` prints these counts after it unpacks (`programs: … payloads in … containers`, then one
line per stage when it is given the carve index, whose `stage` field it reads). It still writes one program per
container, the last payload by default, so the 195 export-stage variants are not translated; which payload a
geometry-shader pipeline selects is open ([fssm_crosscheck.md](fssm_crosscheck.md) §6).

Measured on an own 1.0.2.1 copy from the build's `index.json` and `fssm_index.json`. The synthetic case is
`test_program_counts_separate_containers_from_payloads` in `tests/test_fssm_unpack.py`.

## 4. Tool

```sh
python -I tools/shader_library.py <package>/shaders.rpf --out <work>/library --disasm --carve <work>/shaders/dxbc
python -I tools/rdna2spirv.py <work>/library/disasm --out <work>/library/spirv
```

* It writes `fspx/<name>.code` and `fspx/<name>.json`, the layout `fspx_unpack.py` writes and `rdna2spirv.py`
  reads (`native_blob`, `cs_dispatch_state`). A compute program also gets `fspx/<name>.state`, its record's state
  blob (FSPX kind 0) as it is, whose SHA-256 is the index's `fspx_state_sha256`; in the 1.0.2.1 `shaders.rpf` library,
  every compute program has one distinct state-blob index across `library.json`'s `compute` table (2,684 programs,
  2,693 records). A program that records use with different state blobs is written once per distinct state, the
  first under its name and each further one as `<name>_state_<blob index>`, so each pair of keys has an entry
  (the library measured above has no such program). It also writes `index.json` with each program's stage, and
  `library.json` with the header and both pipeline tables, stage to program name. Programs are named
  `lib_<stage>_<index>`.
* `--disasm` writes `disasm/<name>.s` for the code after the prologue, as `fspx_unpack.py --disasm` does.
* `--carve` names each program's source shaders. It copies the first source container to `<name>.dxbc`, because
  the translator reads the stage's signatures and `PSV0` from there. Pipeline compilation keeps the D3D12 stage
  interface.
* `--save` writes the reassembled library. A saved file is also accepted as input.
* `--stages` limits the output to some stages.
* `--compute-hashes FILE` instead writes only the compute programs whose code has one of the SHA-256 digests listed
  in `FILE` (`fspx_code_sha256`), with every state they are used with. A digest no compute program has is an error.
* `tools/pso_code_base_pairs.py` reads the same library, in either form, and joins each graphics record's stage to
  the translation index through that stage's source digest (§3). It counts whole PSOs by the `uses_code_base` of
  their stages: counts only, no name or code. §6 and [d3d12_vulkan.md](d3d12_vulkan.md) §5.2 report what it says.

The output is game data and must stay in your work folder. `tests/test_shader_library.py` builds libraries and
archives by hand and covers the header, both tables, the size checks, the search in an archive and the digest
walk. It also checks the `--carve` copy, the translator's reads of the compute sidecar and the compute `.state`.

**In the build.** `make_game.py`'s `shaders` stage runs this for the compute stage after the FSPX step
(`library_shaders`): `--stages cs --disasm --carve` into `<work>/shaders/library`, then `rdna2spirv.py` into
`<work>/shaders/spirv/library` with the stage's `--wave64-emulate` and fallback flags. The runtime's shader library
reads that index next to `fspx/` and finds its entries by `fspx_code_sha256` and `fspx_state_sha256`
([fspx.md](fspx.md), "Build and run-time lookup"). That is how the executable creates these pipelines: it reads the
library through DirectStorage zlib requests and passes each compute record's four parts to Xbox device slot 67,
`DeserializeComputePipelineStateX`, as kinds 0, 1, 6 and 7, so the code key is the program's bytes as the table gives
them and the state key is the record's state blob. The step is redone as a whole when `shaders.rpf`, the tools,
`llvm-mc`, the wave64 mode or the flags change (`library_build.json`, written last), and a clash of both keys between
different modules fails it, as for the FSPX step. Graphics pipelines are not translated by the build.

### Resuming a compute-only batch on Linux or native Windows

Run `tools/shader_library.py` with `--stages cs --disasm --jobs 2` and the carved containers, or select the
kind-6 hashes with `--compute-hashes`. For an unpacked library with its input `index.json`, use the compute batch.

```sh
python3 -I scripts/translate_pipeline_compute.py /path/to/unpacked --out /path/to/spirv/library --wave64-emulate
```

Use a dedicated output folder and a stable, trusted SPIRV-Tools installation on PATH. On Linux the two workers
run at nice 19. Each has a 768 MiB address-space limit and uses one SPIRV-Tools process at a time (four processes
total). On native Windows CPython, a BelowNormal coordinator and up to two gated workers run with a 768 MiB
**committed memory** cap per process (not total virtual address space). Each worker's kill-on-close Job permits
only itself and one tool process. The parent verifies job membership before sending work; the child rechecks the
effective limits. A no-share file open keeps the pack exclusive through worker exit. An incompatible outer Job
or a failed limit/lock check stops the build; there is no unbounded Windows mode. A hard stop can leave temporary
scratch files, but cannot partially replace the index. `--jobs 1` uses a single translating worker and
`--memory-mib` changes the per-process limit. A rerun checks each module against its checksum and the listing, source container, FSPX
sidecars, toolchain and wave mode. It retries failures. Only validated modules enter the runtime index. Its
atomic replacement follows both workers, so an interrupted run leaves the previous index readable. A partial
run exits nonzero and writes the reasons to `failures.json`, keyed by kind-6 and kind-0 SHA-256. Old modules
can still be used by a runtime that already opened the preceding index.

The coordinator and each shard explicitly apply `--wave64-emulate`, `--loop-limit`, `--descriptor-hash` and
`--compact-spirv`; requesting compaction requires both translator support and `spirv-opt`. The cache key includes
those settings, compiler sources, builder and tool hashes. `.compute-cache/worker-receipt.json` records the exact
options and input-name digest for each shard; compare its `index_sha256` with the actual published index before
using it. This is an audit file, not a completion marker. A pre-upgrade cache key is not reused as evidence for
a new builder.

Among 2,680 compute programs, 46 start with two 12-byte literal-bearing VOP3 NOP hash carriers. Their body
starts at byte 24. The three-record form starts at byte 36. The no-op measurements are in `docs/fspx.md`.
A hash literal can resemble an instruction. `shader_library.py` excludes both complete carrier records before
looking for the end of the program. LLVM input uses one dword per line, including within multiword instructions,
so its diagnostic for an invalid word echoes at most four source bytes.

## 5. Translation

With `--carve` and `rdna2spirv.py` at `main` `aa2988cc` (llvm-mc 23, `gfx1013`), **17,955 of the 21,522 programs
translate and pass `spirv-val`**:

| stage | programs | pass `spirv-val` |
|---|---:|---:|
| VS | 1,343 | 1,333 (99.3%) |
| MS | 7,498 | 7,059 (94.1%) |
| PS | 9,996 | 7,719 (77.2%) |
| CS | 2,684 | 1,844 (68.7%) |
| slot 6 | 1 | 0 (stage not identified) |

What stops the rest, by the translator's message:

* **2,533 programs: "reachable instruction with normalized encoding".** llvm-mc rejects scalar register tuples that
  run into `vcc` (SGPRs 106 and 107): `s_load_dwordx8`/`x4`/`x16` writing `s[100:107]`, `s[104:107]` or
  `s[92:107]`, and image instructions whose resource is `s[100:107]`. In the compute programs alone that is 37,952
  SMEM, 22,248 MIMG and 2,298 MUBUF words. The same words with the register field one tuple lower decode; with
  `gfx1030` they don't either. The FSSM shaders don't use these tuples; the pipeline builds use them often. The translator
  now decodes these words from the blob with `rdna2_decode` (`restore_vcc_tuples`) and reads them as the
  `[s100,...,vcc_lo,vcc_hi]` lists it already takes (design.md *Scalar/buffer memory*); synthetic cases are in
  `tests/semantic/test_vcc_tuple_fallback.py`. The junk llvm-mc prints at such a word's second dword is often longer
  than that dword (an 8-byte `exp`, say) and swallows the instruction after the tuple too, so the words after a restored
  instruction are decoded from the blob until the stream is back at one llvm-mc printed. A word llvm-mc itself rejected
  is not taken for swallowed code and stays refused. They are decoded for the program's own wave size: in wave64 a
  swallowed VOP3 compare writes an SGPR pair. The table above was measured before both changes. Measured since,
  over the 2,684 compute programs (llvm-mc 21.1.8): 1,816 translate with the tuples alone and 2,567 with the swallowed
  instructions, all of which pass `spirv-val`; the modules of the 1,816 are byte-identical.
* **443: an instruction llvm-mc prints with another encoding than the blob's** (`v_fma_mix*`, `v_interp_p2_f16`):
  the existing `check_printed_encodings` refusal.
* **90 CS: "CS thread-group-size SGPR"**: RSRC2 `TG_SIZE_EN`, which no FSSM compute shader sets.
* **68 translate but fail `spirv-val`**, mostly "requires a previous definition".
* Fewer than 30 each: `ds_append`, `s_bitreplicate_b64_b32` and `s_flbit_i32_b64` on a lane mask, `v_cmp` and mask
  destinations, 25 branches into data, 10 undecodable words that can't be re-anchored, and a few `vcc`/`ttmp`
  operands.
* Programs with no source container: a PS or MS one has no signatures and is `Unsupported` (the reads now raise
  that instead of `FileNotFoundError`, which stopped the whole run). A VS that fetches no inputs translates without
  one.

None of the programs has been run on a GPU.

## 6. What this changes

* **Pipeline pairing is in the package.** [d3d12_vulkan.md](d3d12_vulkan.md) question 14 asks whether a PSO pairs
  two code-base stages, and says the pairing is in the executable. The graphics records give it for 25,471
  pipelines: each names its VS or MS and its PS, and each source can be found through §3. `tools/pso_code_base_pairs.py`
  joins that to the FSSM translation index and counts the result: **no PSO in 1.0.2.1 has two code-base stages**.
  25,027 have none, 444 exactly one (343 MS, 60 PS, 41 VS), and 21 stay unknown because a stage's program is not in
  the carve output. So the one code base in the 144-byte push-constant block is enough for this package.
* **A second compiled form of 6,522 shaders.** The pipeline builds differ from the FSSM code. Which form the game
  runs for a pipeline is not measured here (that needs the executable). Either way, a translated pipeline build can
  be compared with the translated FSSM shader of the same pipeline.
* **Not decoded.** The graphics state blob, the 40 bytes in each graphics record, the compute kind-1 blob, the
  records inside the stage blobs, and slot 6.
