# What was learned on the RX 7900 XTX (2026-11-23 to 2026-11-25)

One PC: i9-12900K (8 fast cores, 8 slow ones, 24 threads), 64 GB, Radeon RX 7900 XTX
24 GB, Windows 11, 1440p, Series X profile. Figures are of diagnostics builds with a
screenshot a second. Scenes are not the same from run to run, so read differences
under about 5 % as nothing. What a switch does is in the comment where it is read.

## Where a frame goes

Median of whole runs, same part of the prologue: 280 ms, then 124, 103, 75, now 46-65.
On the water (first boat mission) 46-53 ms.

The title needs 33 ms. Of a 55 ms frame on the water:

- **The thread that prepares submissions: 48 ms.** It is the wall. Steps of the lists
  18 (refreshes of host copies 10), steps done on the card 7, waiting for descriptor
  records 6, asking the write watch 5, copies in the card's memory 4, the title's bytes
  under shader writes 4.
- **The card: about 41 ms busy**, the next wall.
- **List calls: 36,000 a frame, 230 ms of work** spread over eight threads. In a street
  chase 67,000 calls and 430 ms.
- Descriptor records copied as plain bytes: 400,000 slots looked at, 40,000 mirrored a
  frame, most of them empty.
- Landing what shaders wrote: 16 MB a frame put as dwords, on a thread of its own.

## The one idea behind most of it

On the console the CPU and the graphics processor share memory. Here they do not, so
everything one writes and the other reads must be noticed and moved. Every large cost
is one of four: **noticing** writes of the title (write watch, guards), **noticing**
writes of shaders (hashes on the card, compares on the CPU), **moving** them, and
**waiting** until an order is safe. Gains came from making each of them narrower
(dwords, not pages), wider (threads, AVX2) or later (on the card, behind the frame),
in that order of size:

| What | Frame before | after |
|---|---|---|
| Descriptor records of draws' tables left to the draws | 280 ms | about 124 |
| Our hash passes over card copies, 64 threads a piece | card 79 ms | 66 |
| Preparing on a thread of its own, refreshes and records spread | 103 | 75 |
| List calls written down and translated by eight threads (`LEONIDA_THIN`) | 75 | 68 |
| Submissions joined, card steps, textures sent ahead | 68 | about 63 |
| Title bytes under shader writes compared with AVX2 on six threads | 14 ms of it | 4-7 |
| Stores into guarded memory looked at first, fewer guards reset | 69 ms of threads' time | about 6 |

(Each figure is from the run in which it was measured; they overlap and do not add up.)

## Faults found, and what they were

- **Boats sank, the sea plane went under.** The ocean's height and displacement maps are
  textures that compute passes write and the title's physics sample on the CPU. They were
  never brought back to title memory. Fixed by Geckomacabre's read-back as merged in
  fxceless/victory-fork f5c03f3b; on by default here (`VICTORY_TEXTURE_BACK=0`: off).
  With it the boat stays up through a whole run; without it sank in every one.
- **A device loss about every 16 minutes**: a page fault inside a texture made in the same
  tenth of a second. A slot showed it before its upload had run.
  `LEONIDA_SHOW_AFTER=1`; none in the sessions since (likely fixed, not proven).
- **A rare lost write** (6 of 200 runs of a stress test): a view was mapped between our
  look at its guards and our lock, and views were opened one after another. None in 603
  runs after.
- **Blocky patches**: indirect dispatches cut at 65,535 groups.
- **Square holes and dashed edges in shadows**: pixel shaders that read their quad's
  neighbours with wave reads; on AMD helper lanes take no part in them.
- **Cube reflections too blurred, wrong materials**: DXIL's `Log` is base 2, and
  normalized buffer codes were one step off.
- **Every ray missed**: the dispatch flag that says counts are threads, not groups.

## Measured and not worth it

- Unordered early pipelines for everything (AMD's compiler crashed).
- Series S profile, smaller textures, a second drawing queue, `ASYNC_SUBMIT` off.
- Mapping pages on sixteen threads: no more pages mapped, frames longer.
- Scalar buffer loads four dwords a call (`LEONIDA_SHADER_OPT=511`): no gain on this card.
- Wave sizes, fused multiply-adds, fast reciprocals, raw picked buffers, leaving out
  barriers: nothing to 0.1 ms each. Exact RDNA float rules (`LEONIDA_RDNA_FLOAT`): 0.8 ms.
- Running the console's shader code natively on the card: the Windows driver takes no raw
  code, the two generations differ, and a Vulkan host layer ends at the same compiler. What
  is left to gain from shader code was guessed at under 1 ms.
- An on-screen panel of numbers (`LEONIDA_PANEL`): it helps finding where a frame goes but
  costs some, so it is off and F3 shows it.

## Translated compute programs on an RTX 4060 Ti (2026-11-27)

Counted offline over the 2,703 compute programs of the title that translate (the
`translate_cdxil` example: `--census`, the NVIDIA host settings, every translation also
through `dxil.dll`'s validator). The card time of a frame barely moves between 1080p and
1440p, so what counts is per group and per command, not per pixel. What the clean-up and the
wave64 forms took out, as instructions of the written module and group barriers:

- Exchanges of a console wave64 program in two host waves of 32 that run once
  (`LEONIDA_EXCHANGE_SITES`, dwords of their own, one barrier): barriers 10,643 -> 9,156.
- A first-lane read of a load from a read-only view at a lane-uniform address needs no exchange
  (`LEONIDA_UNIFORM_LOADS`): 9,156 -> 9,108.
- `LEONIDA_SHADER_OPT` bit 128 (a value kept for the lanes a guard leaves out, read only where
  the guard holds, is its new value): guard selects 488,284 -> 281,592, instructions -1.5 %.
- `LEONIDA_SHADER_OPT` bit 256 (a conjunction of lane conditions with its terms in the order
  they were made, so the enclosing branches' share is one value).

What is left, by size: exchanges inside loops (two barriers each: about 110 of the 134 barriers
of the 8 x 8 material program `1f1fbd86759b3da2` are in loops), the entry ticket of every
console wave64 program with lane code (a group barrier and an LDS atomic, also for groups of
one dimension, where the host's waves are runs of consecutive ids), descriptors made again in
every branch of a program (`createHandleFromHeap` of an index that is a root constant plus an
offset, behind a branch on a load), and reads of read-only views repeated in blocks that
follow one another (70 of 118 in `922e1d0ee1e19a85`).

## Console wave64 programs a 32-lane host refused: the water's tiles and the culling program (2026-11-27)

26 of the 2,734 compute programs of the title were refused on the RTX 4060 Ti ("no wave32
lowering for this wave64 program") and every dispatch of them was dropped. Five were taken
apart with `LEONIDA_WAVE_WHY=1` and `translate_cdxil`; what each trips on, and what takes it now:

- **`5dc3b25986622949`, `6f225b641c5b0708`, `926c46bab3b86bd6`** (the water's
  `CS_TransmissionVolumetrics`; 8 x 8 groups of one console wave, 512 bytes of LDS), and four more
  of the family (`2889c21328c69e58`, `6871d6e4b3ac9a10`, `888af37a584acf98`,
  `a10179ab7eda0dc3`): a lane's early return (`s_and_saveexec` and `s_cbranch_execz`) around code
  that writes and reads LDS (`ds_write2_b32`, `ds_min_u32`, `ds_max_u32`, `ds_read2_b32`, at
  `0x230` and `0x404`-`0x4c4` of the first) and takes wave reductions and first-lane reads where
  not every lane runs. Thread (0, 0) stores the tile's starting maximum and minimum after those
  returns. Taken by the branches wave-wide, and by `lds_init`.
- **`6b82bc7d22016cb5`** (culling, 1,024 threads in one dimension, 32 KiB of LDS; its sibling
  `6832a9da15c2e584` too): LDS read and written in lane-branch regions (thread 0's
  read-modify-write of a counter at `0x36dc`-`0x3704`; the allocation loop's bitset read and
  `ds_add_rtn_u32` at `0x39c4`-`0x3a14`); a reservation by an LDS atomic, which the allocation
  idiom did not know (only buffer atomics); and one LDS dword over D3D12's 32 KiB
  (`LDS-OVER-32K`: the dword of `wave_base`'s counter, and two exchange dwords for each console
  wave, of a program that has all of the 8,192 dwords itself). Taken by `lds_exact`, the idiom
  for `ds_*` atomics and no counter.
- **`02384a60fda6f2d0`** (token compaction, 128 threads in one dimension, two console waves):
  not taken, see the end.

**Wave-wide branches.** The console takes a branch on EXEC for the whole wave of 64: the body
runs unless no lane is left. `emit` has had that form (`widen`: each host wave asks its lanes,
the halves exchange the answers, so both go the same way and can meet inside) behind
`LEONIDA_WAVE_WIDE=1`. It is exact, so it is now the last form tried for an 8 x 8 group (after
every other one, the approximate ones too: no program that translated before changes; 4, 6 and
20 branches of the three programs above are taken so, every lane running the code with its EXEC
bit clear where it has no work). `LEONIDA_WAVE_WIDE_LAST=all` takes the 1-D groups of 64 as well:
13 more programs of the title translate and validate that way (2-30 KiB of LDS), but none has run
on a card yet and runs 121-124 lost the card with the form on for the light atlas's programs, so
they stay refused; `0` takes none, `LEONIDA_WAVE_WIDE=0` turns the form off everywhere. With the
form on from the start (`LEONIDA_WAVE_WIDE=1`) 33 programs that translate without it change,
which is why it is not the default.

**LDS starts (`lds_init.rs`; `LEONIDA_LDS_INIT_HOIST=0` off).** Every lane `ds_max_u32`s and
`ds_min_u32`s its key into two dwords that thread (0, 0) set to 0 and `0xFFFFFFFF` after the early
returns, with no barrier (one console wave). When (0, 0) has returned nothing stores and the
atomics combine with what the LDS held: another group's values on the console, anything on the
host (a dark patch in a lit tile: the "dark tiles" of the wave-wide form). A dword that every
write sets to one constant and every atomic accumulates into with that constant as its identity
is stored by every thread first thing, behind a group barrier. That is exact where the dword is
defined: where the program's own store runs it stores the same value again before the first use,
and a read that would see the early store instead of a later write of the group would have read
memory the group never wrote. Confirmed on the card with a hand-written 8 x 8 fixture
(`water_tests`): with the corner thread returned, the minimum came out as what the LDS held (0)
without the hoist and as the others' minimum with it; a start that is not the maximum's identity,
a write at a computed address, or no atomic hoists nothing. Ported from fxceless/victory-fork
a918163 (GPL-3.0). Its paired FP32 and wave64 sums (`paired_fp32.rs`, `wave64_after_loop.rs`) are
not needed here: the sums of these programs (the count of the lanes as `f64` and `f32`, a sum of
`0.0` over the launched lanes, lanes that have left included) are the tree sums `wave.rs` replays
bit for bit with lanes left out (`wave64_tests`: "a sum of halves where lanes have left").

**LDS regions that need no order between the halves (`lds_exact.rs`; `LEONIDA_LDS_EXACT=0`
off).** A lane-branch region that reads and writes LDS was refused unless it only raised a flag
(a group barrier cannot stand inside it). Two more are taken, in the last forms `emit` tries (after
the wave-wide one, each form whole and as the allocation idiom's local waves), so that a program
that translates without them keeps its form. A region **one thread runs**: every
way in is the taken edge of a branch on `v_cmp_eq(thread id, k)` in a group of one dimension, or
comes from a block that thread alone runs (a loop's mask joined by a phi is followed); its reads
and writes are one thread's, in its own order. And a region whose **reads and writes cannot
meet**: the byte range of each access is bounded from how its address is made (a constant, a
mask, a shift, a sum, the access's own offsets); no range read meets a range written, and atomics
that meet are the same operation, so no order of the halves can show. An atomic that returns the
old value (`ds_add_rtn_u32`) only in the form of `with_local_waves`, whose idiom check (a wave's
count goes to the first lane's atomic, the lanes take the base it returned plus their rank among
the lanes) makes the order of the halves legal; that idiom now knows `ds_add_u32`, `ds_min_u32`,
... as the reservation. A group of one dimension whose LDS fills all of the 32 KiB takes its
console threads from the flat id (no counter dword), and in the idiom's form gives no exchange
dwords, which it never touches. Confirmed with hand-written 128-thread fixtures on the card (two
console waves): thread 0's three read-modify-writes of a dword; the same by threads 0 and 1 (no
one-thread region: the form without the skip gives it a barrier per access); the odd threads'
wave allocation through an LDS counter (64 slots, each once, 8 runs) and the same in a loop the
lanes leave one by one (192 allocations, each once); and offline over the corpus
(`translate_cdxil --census`, every translation through `dxil.dll`'s validator): 22 programs are
still refused, none invalid, and the instruction counts of the others change only where a hoist
adds its stores. `6b82bc7d22016cb5` translates and validates in the form with the idiom (exact
in the sense of `with_local_waves`: each lane a slot of its own; the order of the slots between
the halves is any).

**What is left: `02384a60fda6f2d0`.** Its counter in LDS is read by every lane of a console wave
and then read and written by lane 63 alone (`ds_read_b32` at `0x254`, `v_mbcnt` ranks,
`ds_write2st64_b32` at `0x288`, `ds_read_b32` and `ds_write_b32` at `0x2b4`-`0x2c4`), in a loop
each console wave runs for as many rounds as it has tokens (the second wave in another loop at
`0x364`-`0x5d8`, with a counter of its own). The halves have to be in step at both places of
every round: no group barrier gives that (the other console wave is in a loop of its own, with
another count of rounds), and the idiom does not cover a counter updated without an atomic. What
would: the wave-wide branches for groups of several console waves, the two host waves of one
console wave meeting through a pair of LDS flags (each half counts the meetings it has entered
and waits until the other's count is not below its own) in place of a group barrier. Not
written: it spins on LDS, which no run on a card has tried, and it needs LDS for the flags.
`LEONIDA_WAVE_LDS_LOOSE=1` takes the program, approximately (the halves meet only around the
loops): wrong for the counter.

**What the wave-wide form costs on the card.** In a GPU timing run at the save (Series X,
1440p, RTX 4060 Ti; `LEONIDA_GPU_TIMING=1`, 2026-11-27) one indirect dispatch of
`6f225b641c5b0708` took 76-80 ms of the frame, every frame; the next heaviest command took 3 ms
and all the others together about 65. Its translation (`dxc -dumpbin`) has 125 group barriers, 115
of them inside an outer loop that holds nearly the whole program and 50-76 inside each of two loops
nested in it: each branch on EXEC and each wave read where not every lane runs is an exchange of the
two host waves through LDS with two barriers, every iteration, in every 8 x 8 tile of the screen.
Runs x31-x33 had looked right with the water fix, at about 35 ms on the card: the program's
pipeline was not made yet in them (dispatches without a host pipeline are dropped), so what they
show is the water without it. The NVIDIA launcher drops its dispatches again
(`LEONIDA_WATER_TRANSMISSION=1` runs it). What would make it cheap: one barrier per exchange (two
dwords used by turns), and no exchange for a branch whose condition the console wave already holds
in an SGPR (uniform by construction).

**Paired lanes (`paired.rs`, `LEONIDA_PAIRED_LANES`).** Ported from TaynanGT/Vice-Tay 5e588e8
(Geckomacabre with fxceless, after ByFxbian's fxceless/victory-fork #1397 and #1419, PiloteCode #854
and darkworldhm 840243e2; GPL-3.0). One host wave of 32 threads runs one console wave of 64: each
thread lowers console lane `n` and lane `n + 32`, every instruction for the lower lane and then the
upper one, every per-lane branch taken by the whole wave (as in the wave-wide form). A reduction, a
ballot, a first-lane or lane read and a wave-wide branch ask the host wave for both lanes of every
thread, so nothing is exchanged between host waves; the DPP rows, swizzles, `v_permlane(x)16` and
`ds_bpermute` stay in their 32-lane half, which is the lower or the upper lanes of the host wave. LDS
is the program's own, each access bounded by it, behind a group barrier where a write or the first
read of a block comes (the host's lanes of a wave are not in step). The form takes dword reads and
writes (`2` forms too) and the order-free atomics (`ds_min_u32`, `ds_max_u32`, `ds_add_u32`, ...); it
refuses spilled programs, groups that do not fill their console waves, and several console waves
of a group that would have to meet after they went their own ways. Not taken from Vice-Tay: its
replay of the console's order for 32-bit float sums (`paired_fp32.rs`; sums are joined as the
wave-wide form joins two host waves', exact for the water's counts), the programs by hash
(`VICTORY_WAVE_PAIRS_ONLY`) and its loop-exit change to the wave-wide form.

It is tried right before the wave-wide form (`LEONIDA_PAIRED_LANES` unset or `1`; `0` never;
`first` first of all the forms, for trying it on programs another form takes). Over the 2,734
compute programs (`translate_cdxil --census`, `dxil.dll`'s validator on every translation): the
seven programs of the `CS_TransmissionVolumetrics` family change and no other, none invalid, the
same 22 refused. Group barriers (all of them outside loops now; `dxc -dumpbin`) and DXIL instructions,
wave-wide form -> paired: `6f225b641c5b0708` 125 (76 in loops) -> 5, 12,072 -> 22,288;
`5dc3b25986622949` 111 (58) -> 5, 7,966 -> 14,129; `926c46bab3b86bd6` 703 (376) -> 10, 38,260 ->
70,226; `2889c21328c69e58` and `888af37a584acf98` 138 -> 2, `6871d6e4b3ac9a10` and
`a10179ab7eda0dc3` 293 -> 9. Each thread does two lanes' work, in half as many threads. With
`first` 172 programs change (none invalid, two more of the refused translate). Hand-written fixtures
run on an RTX 4060 Ti (`paired_tests`): counts, first lanes and ranks after a loop lanes leave apart,
LDS one half writes and the other reads with no barrier (one and two console waves), the tile's
minimum and maximum and an LDS count over the lanes that go on, a wave OR and `v_readlane` of lane
37, `ds_bpermute` with EXEC per half, the 16-bit tree sum with lanes gone, a mask's "some lane" as
data, the console threads of an 8 x 8 group: all as the console computes them. Not yet measured in
the game (`LEONIDA_WATER_TRANSMISSION=1` with GPU timing).

## Group packing (`LEONIDA_GROUP_PACK`, off by default)

The card's time barely moves between 1080p and 1440p and is far above what the instructions of
its programs cost (`ffdb43445541c981`: about 0.08 ms of arithmetic, 2.9 ms measured), so the
suspect is launch and occupancy: 1,558 of the 2,703 compute programs use groups of 32 threads or
fewer (1,460 of exactly 32), some with 57,600 groups to a dispatch, and an Ada multiprocessor
holds 24 groups, which are 24 of its 48 warps when a group is one.

`LEONIDA_GROUP_PACK=<K>` (2, 4 or 8; the largest of them not above the number; 0 or unset: off)
translates a program of one console wave of 32 lanes with no group barrier to a host group of
`K` such waves (`numthreads(32 * K, 1, 1)`):

- host thread `t` is console thread `t % 32` of slot `t / 32`; the console thread's x, y, z are
  taken from the console group's shape; each slot is a host wave of its own (a one-dimensional
  group on a host of 32-lane waves: `lower::Pack`);
- the console group number is `(x + 65535 * y) * K + slot` of the host group (X, Y), taken apart by
  the console's grid, which the host puts in three root constants (`USER_PACK_X`, `USER_PACK_Y`,
  `USER_PACK_GROUPS` of `shader.rs`; their metadata `dword` is `K`, so a cached translation says
  it is packed). A thread whose number is not below the groups in all leaves at once, which is
  right because there is no barrier;
- group-shared memory is a slice for each slot (`slot * L` dwords added to every LDS address,
  `K * L` dwords in all, at most D3D12's 8,192); `K` comes down until it fits, and until
  `K * 32 * vgprs` is 32 K registers or less;
- the host runs `pack_grid(groups, K)`: the console's groups in all divided by `K`, rounded up,
  as `(min(n, 65535), ceil(n / 65535), 1)`. Direct dispatches do it in `State::dispatch`; indirect
  ones in the clamp (`indirect::bound_packed`, the batched clamp's job word 3), which writes the
  console's grid and the host's dispatch as one 24-byte record, executed with a command
  signature of three root constants and a dispatch (`draw::pack_signature`).

Not packed, with the counts over the 2,703 (`translate_cdxil --census` prints `pack K` or `pack 0
<why>`): 1,237 groups of other than 32 threads, 41 dispatches counted in threads (the structure
builder's), 28 console wave64 programs. **1,396 are packed** with `LEONIDA_GROUP_PACK=4` (1,395 at
4, one at 2 for its group-shared memory): 1,229 of 32 x 1 x 1, 146 of 8 x 4, 15 of 4 x 8, 5 of
4 x 4 x 2, one of 2 x 2 x 8; 77 use group-shared memory, 554 use over 128 registers. With `LEONIDA_GROUP_PACK=8`: 837 at 8, 558 at 4
(the registers), one at 2, none refused by the validator. The programs
`ffdb43445541c981`, `cb6adcc6da2613ff`, `5cf00dc39042edaf` and the tile lists'
`2dc364df5d3278b8` and `940c2635dc1312f4` are among them. All of the packed translations pass
`dxil.dll`'s validator.

What it does not do, and what to look at: a program with a group barrier is not packed (the
slices would have to meet at it, and the tail of the grid would have to wait there); groups of
fewer than 32 threads are not (a wave would hold several groups); wave64 programs are not.
Shaders with lane operations are packed (each host wave is one console group where the host has
waves of 32 lanes, as it does here): that rests on a 1-D host group giving a wave a run of 32
consecutive thread ids, which `LEONIDA_GROUP_PACK=0` takes away again if a program misbehaves.
The translator already takes a console group-shared memory address as in bounds (a shortcut);
packed, an address past the group's memory reaches the next slot's instead of the host's end.

## Three picture faults looked into without the game (2026-11-27)

From the code, the forks' histories, the frame listings of runs x2 and c1 and counts over a
work folder's programs (26,503 disassembled, 12,101 of them pixel programs). Nothing here
has run in the title yet: each line says what would show it.

**A light rim around silhouettes in fog.** `LEONIDA_STATIC_SAMPLERS_LINEAR=1` made every
immediate sampler linear. 21,876 sample instructions in 1,468 program names use a point
one (gathers apart). The pass that puts the fog on the picture,
`CS_ApplyCombinedDeferred` (960x540 ray-marched fog and depth onto 2560x1440), fetches the
four low-resolution texels around a pixel through such a sampler, depth first, and keeps
those within a quarter of the pixel's depth; `CS_TAA2_Resolve` has ten point taps.
Filtered, each tap is a mix across the silhouette. The reason for the setting is gone:
the bands in the colour grading were a pipeline restored from the pipeline library with
another run's sampler places (trustno1-coder/VIctory-NVIDIA 7fa0e95), and such pipelines
skip the library since f0606a744. The launcher now names the tonemap's programs alone
(`PS_Composite`). To check: fog behind a tree or a wire, with the launcher as it is and
with `-More @{LEONIDA_STATIC_SAMPLERS_LINEAR='1'}`; the sky's gradient for bands with
`'0'`. Not the cause, though the frame line counts it once a frame ("depth planes ... used
before the pass"): that use is `CS_Tiling_Generate_Opaque` reading the stencil plane of the
material-class texture, which is copied at once; the plane's readers (the tiled lighting's
draws with the test "equal") are in other lists, after the pass that runs when that list closes.

**Square patches in a texture.** The look that finds a texture changed after its upload
reads 48 KiB of it (`texture::fingerprint`): the smallest levels and two places of the
largest. Such changes are common (200 and more a run found by those windows; 18 of the
first 200 of run port15 more than 176 frames after the upload), so some fall between the
windows and stay. `LEONIDA_HEAP_PRINT_SPREAD=1` (off) adds 256 bytes in each 64 KiB step,
64 places at most, 3 us more a look at 2 MiB; the log names what only those places find
("changed where only the spread places of the look read"). To check: a run with it, the
count of that line, and whether the patches still come. The forks' BC7 rotation fix
(fxceless 06702fe8) changes nothing here: none of the table's 512 control words and none of
the 255 dictionary entries has rotation bits outside modes 4 and 5.

**Black squares at a glass door's foot.** 3,023 pixel programs ask for the depth test
before they run (`DB_SHADER_CONTROL` bit 12); the translator read the word and did not
use it, so the 705 first programs of the transparent passes store their tile masks and
nearest depth for pixels the test rejects. `LEONIDA_EARLY_DEPTH=1` (off; after trustno1
41b4a05) declares the early test. To check: the same view with and without it, and the
targets of `PS_AlphaVelocityDepthResolve` in a dump (`LEONIDA_DUMP_TARGETS=1`): the scene
depth at an opaque edge in front of glass. Not ported: trustno1's stand-in for reflections
after `CS_RayMiss` (2db59d4 and five more), which is where the blue comes from.

## Static samplers and the pipeline library on NVIDIA (2026-11-27)

RTX 4060 Ti, driver 616.64, without the game: `pipeline/places/tests.rs` runs the test
program again as processes of their own that share a cache folder, as runs of the title
do, with hand-written programs that read back which sampler they sampled through.

    cargo test -p leonida-gpu --features diagnostics --lib pipeline::places -- --ignored --nocapture --test-threads=1

**The fault reproduces, every time.** A run stores three pipelines with static samplers in
the library (compute by its description, graphics, compute by a stream; root signatures
with the flags the title's have). The next run makes ten other static samplers first and
loads them: all three load, and each of their seven samplers is another one, exactly the
one now at the place its own had in the first run (the first run's had places 0 to 6, the
ten others now have 0 to 9: read back 40, 41, 42 / 43, 44 / 45, 46 for 1, 2, 3 / 4, 5 /
6, 7). Made by the driver in that run instead they are right. So what
trustno1-coder/VIctory-NVIDIA 7fa0e95 found on a 4070 SUPER holds here, and it is not
rare: any other order of static samplers gives it.

**What the driver does** (`the_table_of_static_samplers`: a stored pipeline whose sixteen
samplers had places 0 to 15 is loaded in runs that did other things first, and reads
whatever is at those places):

- A device has one table of static samplers, and a process one device for a card: what
  another handle of the device made before is in it.
- `CreateRootSignature` gives each static sampler it has not seen the lowest free place,
  in the order of the root signature's array. A pipeline's root signature and one that no
  pipeline uses count alike. The sampler's register, space and visibility are not part
  of it: one place however often and wherever it stands. (With a throwaway probe: neither
  is the comparison function of a sampler whose filter does not compare; the border
  colour and the maximum anisotropy are, even where the sampler does not use them.)
- A place is free again when the last root signature with its sampler is released, and
  is the next one given.
- Samplers in descriptor heaps have no place.
- A compiled pipeline holds the places. The driver's own cache on disk knows that
  (`the_drivers_own_cache_knows_the_places`: a compute and a graphics pipeline made by
  the driver took 12.5 and 4.3 ms, 0.7 and 0.5 ms in the next run, 9.6 and 4.9 ms in a
  run that made one other static sampler first, 0.9 and 0.6 ms in the next such run). A
  pipeline library does not: the blob is taken as it was stored.
- So it is the same for `LoadGraphicsPipeline`, `LoadComputePipeline` and `LoadPipeline`,
  for programs compiled by FXC (5.1) and, with the probe, by DXC (6.6, both heaps
  directly indexed).
- Within a process the first of two equal pipelines wins (probe): made, then loaded by
  the same description, the loaded one is the made one and right; loaded first, the one
  made afterwards is the stale one too. A pipeline to hold a loaded one against must
  differ in its code.
- The blob of `GetCachedBlob` differs from process to process in 17 or 18 bytes (16 in
  one run and one or two elsewhere) with the places the same, and in some 420 with them
  different: no plain way to read the places from it. Not used.

**That is where the five minutes went.** Which sampler a title pipeline needs first
differs from run to run (eight workers make them), so the places did, so the driver's
cache missed nearly every pipeline with static samplers at every start, and since
f0606a744 the library was not asked. A sample of the work cache's translations (8 of its
256 folders, some 16,600 programs) has 25 different static samplers (22 in half of it), 9
at most in a program: a short list.

**What is done** (`pipeline/places.rs`; on NVIDIA only, the other drivers' pipelines keep
their names):

1. The samplers seen are listed in `static-samplers.txt` beside the library, in the order
   first seen; the file only grows. When the host device is made, before anything else,
   a root signature is made for a sampler of ours, each listed one in order, 64 more of
   ours and a last one of ours, and all are kept. A listed sampler's place then follows
   from the list up to it.
2. A pipeline whose static samplers were all in the list at the start is in the library
   under a name that also holds the list up to its last sampler (and the list's own name,
   new when the file is begun anew). One with a sampler first met in this run is made by
   the driver, as before, and the sampler is written to the file. It is made at the place
   it will have in the next run (one of the 64 is let go just before, and the lowest free
   place is given next), so that the driver's cache has its pipelines then; nothing rests
   on that (`a_sampler_met_in_a_run_has_the_place_of_the_next` shows it holds).
3. Before the first such pipeline is looked up, every place is looked at on the card: for
   each sampler made at the start a small pipeline, stored in the library by the first
   run that had it there, is loaded and must write bit for bit what the same pipeline
   made now writes (19 samples that tell the bias and level bounds, the anisotropy, the
   three filters, each address mode and the border; 8 for a comparing sampler: its
   function, filter and address modes). If one differs, pipelines with static samplers
   are made by the driver for that run and the log says so. Found in the tests: two
   samplers made on the device before the host had it, and one that is also in the list
   (then only the places before its own are other ones).

`pipelines_with_static_samplers_come_from_the_library` runs it: a first run meets seven
samplers (its pipelines made by the driver in 6 to 34 ms each), the second stores (made
again, about 1 ms each: the driver's cache had them), the third makes ten other static
samplers and a sampler heap first and the pipelines in another order: all loaded, all
right. A pipeline with two new samplers is the driver's in the run that meets them,
stored in the next, loaded from then on, while the others stay loaded.

`LEONIDA_LIBRARY_STATIC_SAMPLERS=0`: none of it, such pipelines are made by the driver
every run as before. `=1`: under their plain names with no care for the places, which in
the same test reads wrong through every sampler.

Costs: the look takes 95 to 155 ms with nine places (two programs compiled, four small
textures). With 320 listed it took 7.6 s in the run that stores the small pipelines and
8.4 s in the first that holds them against new ones (each once: 23 ms a pipeline the
driver has never compiled), then 0.4 to 1.1 s. With some 30 samplers that is about a
second twice, then about 0.2 s, once a run, on the worker that asks first (an estimate
from those figures).

**Not shown here.** That the title's pipelines load in seconds: it needs three starts
(the first lists the samplers and is as slow as before, the second stores and should be
quick if the driver's cache holds 1,650 large pipelines as it held the tests' small ones,
the third loads). The look at the places tells samplers apart by what they do, not by
where they are: two that sample alike in every way it tries would pass for each other
(and would sample alike in a pipeline in those ways too). The debug layer is not
installed on this PC, so the tests did not run under it. The table's size (2,032 on the
4070 SUPER, where the next sampler removed the device) was not measured again; the list
stops at 1,000. If something makes static samplers on the device before the list in some
runs and not in others, those runs fall back to the driver; the log then names the file
to delete to begin anew.

## What is next

1. The preparing thread: split what one submission does over two threads in a row; ask
   for the next submission while this one is sent; view the card's own buffers where fills
   copy them now.
2. The card: small buffers' copies in its own memory (`LEONIDA_CARD_BUFFERS_MIN_KB=64`,
   after fxceless c36971b2; not measured here yet), and the passes that run for nothing
   without rays.
3. Stalls: 270-660 s of a session were full of frames over 200 ms, almost all while new
   pipelines were made. `leonida warm`, and a quarter of the workers for the slow ones.
4. Outdoor flicker: suspected to be one bad indirect record that uses up its list's
   budget, so that every later draw of it is empty. The log's frame line counts them;
   `LEONIDA_INDIRECT_REST=0` is the experiment.
5. Memory, 40-46 GB private: lists' allocators 9 GB, windows 7, uploads 4
   (`LEONIDA_MEMORY_MAP=1` lists it).

## The other repositories, compared

Read up to 2026-11-25: Marvisak/VIctory, fxceless/victory-fork, DexNom/VIctory,
Belobug/VIctory-Belobug-AMD, FQY55kai/ZJOU-VICTORY. What was taken is in the history,
each commit naming its source. In short:

- Marvisak's commits hardly apply any more; nearly all of what they fix is here, often
  another way. Taken: nearest normalized codes, d16 views, shader model flags.
- fxceless: the base. Newest taken: texture read-back, small buffers on the card, hashes
  over a buffer's own bytes.
- DexNom: thread counts of ray dispatches, cube gradients, joined sparse ranges.
- Belobug: pipeline workers for screen passes, the ambient stand-in's idea, pipeline and
  constant buffer bookkeeping (romainhedouin).
- ZJOU: render targets not set again; some shader corrections are being looked at.
