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
- **Blocky patches**: indirect dispatches cut at 65,535 groups. An AMD card was given what the
  title asked from the start; every other card stayed cut until 2026-11-27. On an RTX 4060 Ti
  at the save (Series X, 1440p) the first tile list (`CS_FinalGather_0_1_1_0`) asked for 85,856
  to 86,544 groups of 8 x 4 pixels: its last 20,300 (2,540 tiles of 16 x 16, the lower left of
  the picture behind a stair-stepped edge) got no final gather in any frame, so the railing and
  the character's legs there looked see-through under a displaced copy of other railing. The
  card runs such dispatches whole (65,536 to 2^20 groups, each once under its own number), so
  a card is now asked once a run, with one dispatch of 2^20 groups of a program of ours
  (`indirect::runs_wide`; 75 ms with its compile), and given up to 2^20 groups where it runs
  them; `LEONIDA_INDIRECT_WIDE=0` cuts as before. What is still cut is said in the log
  ("indirect dispatches: the most groups in x one asked for ..."). On the card the list's
  dispatch went from 0.75 to 1.2 ms a frame (`LEONIDA_GPU_TIMING=1`). A form for a card that
  does not run them (the groups folded into rows, the program told its grid) is not written:
  a command signature that sets root constants from the record costs this card 1.4 us a
  command against 0.08, and some 1,800 indirect dispatches a frame would need it.
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
  that trip on the same thing (`2889c21328c69e58`, `6871d6e4b3ac9a10`, `888af37a584acf98`,
  `a10179ab7eda0dc3`; all seven were taken for one family, but by the entry points in their
  containers `2889c21328c69e58` is `CS_GenerateShadowMasks_SampleHairScreenSpace` and
  `888af37a584acf98` `CS_GenerateShadowMasksHairOnly_SampleHairScreenSpace`, the sun's shadow
  mask where hair is sampled on screen, which the NVIDIA launcher dropped with the water's from
  cef91163b until it was noticed): a lane's early return (`s_and_saveexec` and `s_cbranch_execz`) around code
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

**Forms of the tile and resolve programs (2026-11-27).** For the region with a
stair-stepped edge at a glass door (the railing and a character's legs in front of it
see-through, a displaced copy of other railing over the floor): which form the translator
takes, on a host of 32-lane waves, for the compute programs that work by tile or resolve
the transparent passes. Without the game and without a card: the translator alone
(`LEONIDA_WAVE_WHY=1`), over the 81 compute programs one run had made
(`known-shaders.bin`) whose entry points are named `CS_Alph*`, `CS_TileClassification*`,
`*TemporalResolve*`, `CS_RayMiss*`, `CS_ScreenProbe_RayMiss`, `CS_ResolveScreenSpace*`,
`CS_ResolveToMirrorGBuffer`, `CS_*Alpha*` of the traced transparents, `CS_Composite*` and
`CS_TransmissionVolumetrics*`. What a form computes on a card is not in this.

| form | programs |
|---|---|
| as they are | 60, among them `CS_RayMiss`, `CS_ResolveScreenSpace`, `CS_ResolveToMirrorGBuffer`, `CS_AlphaTransmissionHistoryResolve`, `CS_AlphaTransmissionDenoise`, `CS_AlphaTransmissionDepthTileMipDilate`, the eight `CS_SceneColorTemporalResolve*`, the six `CS_ScreenProbes_TemporalResolve*` |
| without early-outs (exact) | 9: `CS_AlphTransmissionPrefilterMip` 2da4beb386509b77, `CS_AlphaTransmissionDepthTileGenerationMip1` 5c6224a6bc3a4066 and `...Mip25` 9bdd33d77fb8ca0f, `CS_TemporalResolveAlpha` 068786c4b7e04851 and 1069ceb574bc92ca, `CS_TemporalResolveWater` 441c0b3940ddfa2d and 780546beec578f2c, `CS_TileClassificationNeighborhoodBoundary` f8a6133bfedc5759 and `...WithLookingGlassMask` 445c61b3089eb8f0 |
| without skips (exact) | 3: `CS_TileClassificationNeighborhood3x3` 2b17036bf3a1d615 and `...WithLookingGlassMask` f83f0ca30c60c3ac, `CS_TransmissionVolumetrics_GenerateTiles` 1aa247ed3653b616 |
| each half-wave its own results (`local waves (APPROXIMATE)`, without early-outs) | 4: `CS_TemporalResolve1_Opaque_FrameStatistics` 0ffd3b911e35e7d0, `CS_TemporalResolve2_Opaque_FrameStatistics` 71883e40620a567a, `CS_TemporalResolveOpaque2` 3c9d23fa83d63d43, `CS_TemporalResolveOpaque_FrameStatistics` 4031c6c05fa7ec48 |
| paired lanes, last of all | 5, the water's: 5dc3b25986622949, 6f225b641c5b0708, 6871d6e4b3ac9a10, 926c46bab3b86bd6, a10179ab7eda0dc3 |
| refused | none |

So no pass of the glass's transmission is refused or approximate here. (edsuuu/VI-BR-PORT's
window glass was `CS_AlphTransmissionPrefilterMip` refused by 32-lane hosts, their 30c29ed:
the access's own guard among what is known of the lanes that read a descriptor, which this
tree's tracer already had.) Four temporal resolves are approximate: what the console
computes once for a tile of 8 x 8 is computed for each half of it. And the forms counted as
exact are not proven so program by program:
that fork found its water's tile list (`...GenerateTiles`) wrong in such a form, whole
tiles unlit, and right in the paired form (cf3394b).

To check, one group at a time (`LEONIDA_PAIRED_FOR=<hash>,<hash>`: the paired form before
every other for the programs named, only those translated again; `shader_cache.rs`): the
transparents' tile list and ray pre-passes (`CS_Tiling_Generate_Alpha` a14c1da4be7b9837 and
`CS_RayPrePass_Alpha_SSR` edb8e66237fc38e6, each host wave on its own as the allocation
idiom, the kind of form that fork found wrong for its tile list; `CS_RayPrePass_Alpha`
d2b169b82f67ec5b); the two looking-glass tile classifications (f83f0ca30c60c3ac,
445c61b3089eb8f0); the four approximate temporal resolves; the two
`CS_TemporalResolveAlpha`. The paired form takes each of these (and the plain tile
classifications, `...Mip25` and the water's tile list), again as the translator says, not a
card. It does not take the two programs of 256 threads (`CS_AlphTransmissionPrefilterMip`,
`...DepthTileGenerationMip1`: its host waves would have to meet), which keep their form.
For the water: `...GenerateTiles` named, with `LEONIDA_WATER_TRANSMISSION=1` in the window's
environment so that the family runs.

**Every compute program of that run by form, and the approximate ones by name
(2026-11-27).** `translate_cdxil --census` with `LEONIDA_WAVE_WHY=1` (the host settings of
the NVIDIA run, no card) over the 2,682 of the run's 2,734 compute containers that carry an
entry point's name. The 81 above came out the same both ways.

| form | programs |
|---|---|
| as they are | 2,449 |
| exact, no group barrier: without skips, without early-outs, without skips with scalars followed | 84, 28, 27 |
| each host wave on its own, the allocation idiom (counted exact) | 30 (two with exact LDS regions): the batch culls, `CS_Tiling_Generate_Alpha`, `_Opaque` 2cc4c197ee29e291 and `_Water` 5e7cd6d3557a6c35, `CS_RayPrePass_Alpha_SSR`, `CS_RayPrePass_Water` and `_Water_SSR`, `CS_MaterialTilesInitial` bad01f53f53248e5, `CS_DilateDenoiseInfo`, `CS_CollectTriangles*`, the liquid trails, the horizon's vehicle lights, the two `CS_Collision*` |
| paired lanes | 7: the water's five and the sun's two hair shadow masks (2889c21328c69e58, 888af37a584acf98) |
| wave-wide | 1: `CS_Visibility` 369cbe906ea72764 |
| approximate (each half-wave its own results) | 34, below |
| refused | 22, as counted before |

The 34 approximate ones, grouped by their names (which pass each belongs to was not looked
up beyond that):

- the sun's shadow mask and what feeds it: `CS_GenerateShadowMasks` bc76c37f9fa3672b,
  `_HairOnly` 0a5ecfee6b79e13b, `_Raytraced` 12b0005a30bc18a9,
  `CS_RTConstantCostShadowsTileClassificationNormalCones` 2620618869072a85,
  `CS_TraceShadowRays_ScreenSpaceOnlyFullRes` eb9a057a81d18c42, `CS_TraceShadowRays_Fullres`
  4e816e1ef600ddcf;
- the lights' clusters: `CS_ClusterLightsWorldSpace` d3e5b737ea7cc150 and
  `...IntensityBased` 55d2bce91ec9abcc (the two that fork names: each half of a cell is given
  half of the lights);
- what shades where no ray hit, the irradiance cache and the shader bins:
  `CS_DeferredFallback_BVH` f8f712930c606707, `_Fallback` 40bc5fd052acfdf4, `_Voxels`
  294e7cdd91ebd1f1, `CS_DeferredFallbacksAlpha_SkyProbe` b7953a05b189dfc1,
  `CS_IrradianceCache_Shade` a53a7977fe762268, `_ShadeSecondary` be14e72d86362031,
  `_IntegrateIrradianceSH` 0a5d4f9047b3dce4, `CS_GenerateShaderBinOffsets` 8dd8d739047fe557;
- the four temporal resolves of the table above;
- `CS_ResolveVolumetrics` 89ed0b981f99573c;
- `CS_Gather` b2711644ca5566cc, `CS_Gather_Reference` 0903802745985664, `CS_GatherDiffusion`
  ce2378e5e8711aad, `CS_GatherDensityAreaForReadBack` 136986812880cead, `CS_ScanMip0` to `3`
  (3c196d81eca7ff15, acf14a70f37fb012, f79824724d8e2daf, f0aa2b1e79275592);
- decals and effects: `CS_ProjDecalResolve_Scalarized` e4a9f58e3acfcf7b and `_Vectorized`
  c4183d3cbee28e8e, `CS_EntityFireResolve_Scalarized` 2e92491c2ae5472b,
  `ptfx_CS_lightAtlas_Sprite` 2561af9577c47289 and `_Trail` c06aa62112ffba4c.

Named in `LEONIDA_PAIRED_FOR`, 32 of the 34 take the paired form (not
`CS_ProjDecalResolve_Scalarized`: a per-lane descriptor written where some lanes may keep
the old one; not `CS_TraceShadowRays_Fullres`: an LDS operation that form does not take); so
do the three `CS_Tiling_Generate_*`, the six `CS_RayPrePass_*` and `CS_MaterialTilesInitial`.
What it costs the card is
not measured: every per-lane branch is the wave's in that form (252 of them in
`CS_IrradianceCache_Shade`, 114 in `CS_GenerateShadowMasks`) and each thread does two lanes'
work; and the two `ptfx_CS_lightAtlas_*` lost the card in the wave-wide form (runs 121-124),
so they are not the first to name.

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

## Frame-rate switches measured on a six-core PC (2026-11-27)

Ryzen 5 7600X (6 cores, 12 threads), GeForce RTX 4060 Ti 16 GB (driver 616.64), 32 GB of RAM, Series X profile,
2560x1440 output, the title's upscaler, at one save point with the idle camera (6,000 to 14,000 draws a frame, so
medians of one run are comparable and medians of two runs only roughly: two runs of the same settings were 75.0
and 82.1 ms). "A/B" means the switch turned every 20 s inside one run (`LEONIDA_AB`).

| What | How measured | Result |
|---|---|---|
| `LEONIDA_PLACED_UNDECLARED` (a copy at its place whose destination no flight declares is the title's own) | A/B, 8 phases | 73.9 ms median with it, 88.5 without; the submitting thread's wait for flights 2 to 4 ms a frame against 6 to 21. On by default. |
| `LEONIDA_PORT_GUARD`, `LEONIDA_PORT_JOURNAL` | A/B, 22 phases | 83.7 ms with neither, 87.4 with the guard, 91.4 with the journal, 88.9 with both. No gain here; off. |
| `LEONIDA_JOIN_PARTS` (8 against 1), with thin lists off | A/B, 11 phases | 112.3 against 114.0 ms: none. |
| `LEONIDA_THIN=0` | one run | 117.1 ms at 9,500 draws, against 95 to 104 at 9,000 to 11,000 with thin lists. Worse; the launcher keeps them on. |
| `LEONIDA_D3D12_BACKGROUND=0` (the driver's background shader work off) | one run | 118.6 ms at 7,200 draws against about 80. Worse: the card keeps the shaders' first versions. |
| `LEONIDA_PORT_CONSTANTS=1` (the title's 32-bit graphics heap in place) | one run | 290 ms a frame (3.4 fps): 31,000 fills a frame are read out of that memory on the CPU. Far worse on this card; off. |
| `LEONIDA_TITLE_GRAPHICS=performance` (the lighter set of the title's own settings) | three runs | 78.6 ms with it, 75.0 and 82.1 without; the draw count does not change at this place. No gain. |
| Render at 1920x1080 instead of 2560x1440 (`-Resolution 1080`) | one run each | 85.7 ms at 10,200 draws with the card 81% busy at 60 W, against 95 to 104 ms with the card 99% busy at 106 W and 15.6 of 16.4 GB of its memory used. |
| A run left alone for 13 minutes | medians per minute | 106 to 115 ms in the first three minutes, 94 to 104 from then on: it does not recover further when the driver's background compiling ends. |

What it adds up to on this PC: about 10 ms of frame per 1,000 draws. At 1440p the card and the processor are both
at their limit; at 1080p the card has room and the frame is the processor's: the process uses 5 to 5.6 of the six
cores, of which (profile by thread group) the title's own threads take 31 to 41 %, the driver's four background
compile threads 26 % in the first minutes, the eight list-recording threads 25 to 35 % and the submitting thread
6 to 8 %, the last being the one thread every submission passes through (about 72 ms of an 87 ms frame).

### What a frame waits for there, and what moved it (later the same night)

The same PC, profile and save. All game starts for these were muted in the Windows mixer (the game's own session)
and unmuted before they were stopped.

**Standing, the frame is the card's.** A timeline of 20 frames (`LEONIDA_TIMELINE`, 96 ms a frame) has the card
running submissions for 80 of the 96 ms and idle for 16; the chain a present came out of is 56 to 59 ms of the card
and 17 ms of the title's own threads, 5 ms of threads woken and not yet running, 3 to 4 ms of write faults. Three
submissions a frame take about 30, 28 and 17 ms of the card, the dozen others 1 to 2 ms each. No thread of the
process is busy for more than 45 % of the time (the submitting thread 41 %, two of the title's 38 %, the eight
list threads 21 to 35 %), with 5.9 cores' worth in use and 17 % of it in the kernel.

**The card's time is spread, not in a few programs.** A timing run (`LEONIDA_GPU_TIMING=1` with
`LEONIDA_D3D12_DRED=1`, every command between two timestamps): 78 to 85 ms a frame in 1,565 kinds of command;
indirect executions 47 to 52 ms in 9,567 commands (5 µs each), direct dispatches 18 to 20 ms in 1,574, mesh
dispatches 6 to 7 ms in 2,300, the copies behind aliased textures 4.2 to 4.6 ms in 86, plain draws 2 ms. The
forty heaviest kinds make 44 ms of it; the heaviest single programs are compute passes of 1 to 4 ms
(`CS_ProjDecalReduceClassification` 4.3, `CS_ShadowRenderTiledCascadesCB` 3.8, `CS_Gather_Classified_0` 2.7,
`CS_TAA2_Resolve` 1.5). Rendering 1080 lines instead of 1440 took 10 to 15 % off the frame (above), so less than
half of the card's time follows the pixel count. What makes an indirect execution cost 5 µs on this card is not
found; the frame replay (`replay/`) is where to ask.

| What | How measured | Result |
|---|---|---|
| `LEONIDA_REMAP_SPANS` (a texture refresh waits only for a change of mapping that touches its own memory) | A/B of 15 s phases while walking and turning, 25 phases | mean 153.3 -> 129.5 ms, median 117.2 -> 103.1, p90 305.1 -> 230.5. Of 130 to 320 named reads a frame, 6 to 128 went on beside a change and none waited. Standing: no change (the lock is held 5 to 13 ms a frame there). On by default. |
| `LEONIDA_BATCH_SCRATCH_POOL` (lists share the scratch buffers of their batched clamps) | one standing run each way, 4.5 minutes in the world | the card's memory in use that no line counted 3,587 MB and rising -> 1,347 MB and level; "card buffers" made 7,165 -> 5,592 MB; host textures made again after being let go 1,700 -> 357; 150 buffers a frame taken from 170 to 200 kept, none made. In the 13-minute session before it the uncounted memory reached 5.0 GB and the card stayed at its budget of 14.3 GB. On by default. |
| `LEONIDA_SECTION_MB` (the title's physical pages in sections of a few MiB instead of one section an allocation) | one run each of 4 minutes walking and turning, every run from the same copy of the autosave | none: mean 111.6 ms, median 105.1, p90 139.3, title memory held for mapping changes 43.2 ms a frame, the mapping thread 46.2. 2 MiB: 91.7, 91.8, 111.4, held 10.6, thread 22.6. 16 MiB: 94.4, 91.7, 113.9, held 8.9, thread 20.7. The card is then busy 90 % of the time (78 % without). The launcher sets 16. |
| `LEONIDA_WINDOWS_BAR_MB` (windows of title memory in card memory the CPU maps) | one standing run each from the same save, with timelines of 20 frames | 0 (all 1,105 windows in the PC's memory): 93.4 ms median, the card running 68 to 73 ms a frame. 3072, the launcher's (759 windows on the card, 797 not): 94.1 ms, 59 to 74 ms. No difference: what the card reads of windows over the bus is not where its time goes. 6144 is not comparable (its view had 4,800 draws). |
| `LEONIDA_ALLOCATOR_TRIM=1` (host command allocators taken anew when 30 s old, idle host lists let go) | one standing run from the same save as the run above, 5.5 minutes | private bytes 30.1 GB against 30.7; 1,624 host lists alive against 1,514, 2,448 made against 1,514; 60 to 70 allocators replaced a second. No memory worth the churn on this driver; off. |
| `LEONIDA_LATE_WAIT=2` (a present settles its frame's flights; asked because El1xr 8fa2de5 counts fewer tile flashes with presents kept behind their frame) | every frame of the standard moves saved and scanned, one run each way from the same save at night | 2 of 481 frames with a one-frame dip or spike as it is, 2 of 462 with the switch, the same early frames in both: there are no flashes at this place to count, so it is not judged. |
| `LEONIDA_BARRIER_MERGE`, `LEONIDA_WIDE_HALF` | A/B of 15 s phases standing, 30 phases, from the same save | neither 78.4 ms mean (median 75.7), barrier merge 78.4 (76.2), half the processors as helpers 78.0 (75.4), both 78.4 (76.3). No gain from either; off. |
| Render at 1920x1080 instead of 2560x1440, again (`-Resolution 1080`) | one standing run each from the same save, with timelines of 20 frames | at the view of 9,000 draws 75 ms -> 64 ms a frame, at the view of 15,000 draws 88 -> 77 ms; the card running 60 to 68 ms a frame -> 47 to 53. By these two views the frame is about 57 ms plus 2.0 us a draw at 1440 lines and 44.5 ms plus 2.2 us a draw at 1080: of the 57 ms some 28 follow the pixel count and 29 do not. |
| `LEONIDA_THIN_THREADS=6` with `LEONIDA_UNDER_THREADS=4` | one standing run | not comparable: the title streamed through the measurement (192 ms a frame of mapping changes held, frames of 160 to 580 ms). To be run again as an A/B. |
| `LEONIDA_PAGE_ASK` (a fault asks Windows about its one page, `QueryWorkingSetEx`, before the region around it; after KyleS98/VIctory-Neo 0fb1e38) | A/B of 4 s phases: standing with a level for each view (97 phases), walking and turning by pairs of phases next to each other (89 pairs); the faults' own line | The question 5.8 us -> 1.2 us, 210 a frame, every one answered by the page: 1.25 -> 0.25 ms a frame of the title's threads. The frame: standing -0.20 ms (+-0.19; means +0.10 +-0.20), walking -0.24 (+-1.05): none. A fault costs 84 us here (780 a frame, 66 ms of all threads' time), which is the wait for the guards' lock (540 waits and 75 ms a frame standing, 117 ms walking) and the change of protection, not the question: the fork's 245 us a question are regions of hundreds of megabytes, and the sections of 16 MiB keep them short here. On by default. |
| `LEONIDA_RETIRE_QUEUES` (objects let go wait for their submission in a queue to each thread, not in one; after KyleS98/VIctory-Neo 9165dd0) | the same two runs; the retired objects' own line | 136,000 to 142,000 objects a frame are retired. In one queue 28,400 pushes a frame find the lock taken and spin 33 ms of all threads' time (41 walking); with a queue to each thread 19 wait, 3.3 ms (4.7). The frame: standing -0.06 ms (+-0.19), walking +0.74 (+-0.98): none. On by default. |
| `LEONIDA_COMMIT_FLOOR=1` (host copies of textures out of use 2.5 s and 60 frames go while less is left to commit than `LEONIDA_COMMIT_FLOOR_MB`; after kylepelham/El1xr c24c0b2 as 22f73d8 mended it) | one run each way of 7 minutes walking and turning, the floor set above what the PC has so that it acts all the time | private bytes 31.0 -> 29.4 GB, left to commit 6.4 -> 8.0 GB, host textures alive 7.2 -> 5.5 GB, the card at 88.3 -> 77.3 % of its budget; host copies made again 166,545 -> 410,045 (400 -> 980 a second); the frame median 84.9 -> 84.5 ms, mean 84.0 -> 85.8, p90 95.0 -> 101.0; the look for textures to let go, once a second on the submitting thread, 2.2 -> 11.6 ms. Off by default: 1.6 GB of commit for two and a half times the uploads. At its 4096 MB the floor would not have acted in any run of the night (5.9 GB left at the least). |
| `LEONIDA_BARRIER_MERGE`, `LEONIDA_WIDE_HALF`, again with short phases | A/B of 4 s phases standing, a level for each view, 96 phases | barrier merge -0.26 ms (+-0.26; means -0.40 +-0.32), half the processors as helpers -0.11 (+-0.26; means -0.06 +-0.32), of 77 ms: none from either, as the phases of 15 s said. Off. |
| `LEONIDA_REMAP_SPANS`, again with short phases | A/B of 4 s phases walking and turning, the third of three switches (21 pairs of four phases) | means -1.5 ms (+-1.5), medians -1.3 (+-1.4), p90 -2.5 (+-2.9), of 87 ms. The 153 -> 130 ms of the row above was measured before the sections of 16 MiB, which took the holds for mapping changes from 43 to 9 ms a frame: 1 to 2 ms are what is left for it to gain. Stays on. |
| `LEONIDA_CARD_AWAKE` (a compute loop keeps the card's clocks up between submissions; the launcher set it; KyleS98/VIctory-Neo 38b768c turned its own off) | A/B of 4 s phases (`LEONIDA_AB` can name it now): standing at 1440 lines (102 phases) and at 1080 (88), walking and turning at 1440 in two runs (89 and 44 pairs) | The frame with it against without: standing +2.14 ms (+-0.30; means +2.07 +-0.33) of 72 at 1440 lines, +0.37 (+-0.32; means +0.61 +-0.32) of 66 at 1080; walking +0.68 (+-1.16) and +1.78 (+-1.84) of 83 to 87. The card is 86 to 99 % busy in all four, so there is nothing to keep awake and the loop takes its share. The launcher sets `0` now: a gain standing, nothing the spread shows walking, and what is given up is the case it was made for (a card idle half the frame, which this one is not). |
| `LEONIDA_STORE_BESIDE=1` (a faulting store of the title's is done beside the guard and the block stays read-only; the same fork measured 32.60 -> 31.17 ms) | A/B of 4 s phases: standing at 1440 lines (102 phases) and at 1080 (88), walking and turning in three runs (21, 89 and 44 pairs); every frame of the standard moves saved both ways | standing -0.56 ms (+-0.30; means -0.90 +-0.33) at 1440 lines and -2.36 (+-0.32; means -2.15 +-0.32) at 1080, where the processor is more of the frame; walking -1.92 (+-0.94), -0.90 (+-1.63) and -1.64 (+-1.45), together -1.7 (+-0.7). 1.4 to 1.7 million stores a run done beside the guards, 56,000 to 75,000 not understood and 30,000 to 47,000 blocks with too many (those open the block as before), no "title memory not written". 553 and 571 frames of the standard moves: the scan flags 2 without and 6 with, the six a fast turn past a white house and the sky with nothing missing in them. The launcher sets it. |
| `LEONIDA_INDIRECT_BATCH=4` (a batch stays open past a program whose every buffer store goes through views its bind declares; a record is clamped again behind one bound to it since the pass), against 1 and 2 | A/B of 4 s phases standing, two switches (`_4`, `_2`; both on is 4), a level for each view, 133 phases; and of 4 s phases walking and turning against 1, 92 pairs | standing, medians against 1: 4 -2.15 ms (+-0.39), both on (4) -2.06 (+-0.39), 2 -3.77 (+-0.38); means -2.07, -1.84, -3.53 (+-0.4 each), of 60 to 82 ms. Walking: means -1.64 (+-1.55), medians -0.60 (+-1.38), p90s -0.74 (+-2.64), of 85. No record ran empty for want of a list's budget in any of the runs (0.00 a frame). 900 to 1,000 batch passes a frame against 1,150 with 1 and 235 to 240 with 2. The launcher's setting from here. |
| The sea's fault (its surface or tiled resolve missing for a frame) in every frame of the standard moves at the waterside, by setting of `LEONIDA_INDIRECT_BATCH` | the runtime's captures: 489 frames with 1, 1,186 with 2, 1,116 with 4; three scans against both neighbours: colour shares gone and new (`seascan.py`, above 0.030), water gone with sand or black new (`seafault.py`, above 0.005), all-black 8x8 tiles; the flagged frames looked at | flagged with 1: 9 / 3 / at most 156 tiles more; with 2: 11 / 10 / 95; with 4: 0 / 3 / 76. The twelve flagged frames looked at are all the camera's own motion (a hand in view, a fast turn, a black ladder against the sky), the sea intact. None shows the fault with 2 either, so the scans are not shown to find it: they say only that 4 is no worse than 1 here. |

The rows from `LEONIDA_PAGE_ASK` down are of 2026-11-28, every start from the waterside save of the next section.
Walking and turning there is no view to fit a level for, so a switch turned every 4 s is judged by pairs: for the
switch of bit b the phases come in runs of 2^b with it off and 2^b with it on, each pair of runs next to each other
gives one difference, and the mean of the differences with its standard error is the effect (`ab_pair.py`; twelve
minutes give +-1.0 to 1.6 ms for the fastest switch of a run, of a frame of 83 to 87 ms). With the launcher as
those rows left it (the card not kept awake, stores beside the guards) a standing run of four minutes has a mean
of 73.1 ms a frame (median 74.1, p90 78.5) where the first run of the night had 76.6 (74.6, 86.1), and 279 waits a
frame for the guards' lock (52 ms of all threads' time) where that one had 540 (75 ms).

**What the PC's memory watch says on this PC** (`residency`; its own line, "memory, the PC's (residency)"). In every
run of 8 to 13 minutes it is under pressure from the moment the world is in and never out of it again (326 of 361
looks a second apart standing, 504 of 539 walking), by physical memory alone: 360 to 390 MB free at the least, with
other programs holding 46 GB, while 5.9 to 7.0 GB are left to commit, above the 4 GiB where pressure by commit
begins. So for the whole of a session texture prefetch is held to 64 MiB a pass, one spare staging block is kept
and spare chunks are trimmed every second. Commit was never critical (0 of 8,300 to 11,400 questions), no texture
pass was refused (of 0.7 to 2.6 million asked for), no new staging buffer (of 105,000 to 492,000), and no allocation
failed. What is left to commit falls by 0.11 to 0.16 GB a minute while walking (7.4 -> 6.1 GB in 11.5 minutes; private
bytes 30.0 -> 31.4 GB): if it went on so, the reserve of 1 GiB for new staging buffers would be reached after some 45
minutes and the 512 MiB for texture passes soon after, and textures would stay away from then on. No run was long
enough to say whether it does go on; a session of an hour with that line in its log would.

**Host copies of textures come and go while walking.** The card then stays at 88.3 to 89.1 % of its budget, just
over the 88 % from which the keep rule lets the longest idle copies go: 281,530 let go and 166,545 made again in 9
minutes (400 a second), 427,483 and 300,043 in 13; standing (the card at 73 to 87.5 %) 33,532 and 1,765 in 8. By the
two commit-floor runs 580 more a second are at most 2 ms of the mean frame, so the 400 are about a millisecond of it.

While the title streams (the idle camera is enough: 850 to 1,600 pages of 64 KiB mapped a second) the mapping
thread holds title memory for 90 to 270 ms of a frame in single holds of 45 to 70 ms, and each page costs it 0.4
to 3 ms, nearly all of it in the kernel's unmapping (`ZwUnmapViewOfSectionEx` is 47 % of that thread's running
samples, `NtMapViewOfSectionEx` 10 %). Whether the title would stream that much on the console at rest is not
known; its texture read-backs (18 a frame) are where its streaming decisions come from.

Over one standing run the frame follows the draw count closely: 77.7 ms at 8,000 draws, 100.5 ms at 14,000, and
70 ms at 4,800 in another run from the same save, which is about 47 ms a frame whatever is drawn and 3.8 µs a
draw. The 47 ms are more than a 30 fps frame has, so the passes that do not grow with the draw count (1,574
direct dispatches, the compute passes of 1 to 4 ms, the copies behind aliased textures) are where to look first.

Runs are only comparable when they start from the same save: the title rewrites its autosave every two minutes,
so a start loads wherever the last one ended (a road junction at midday, a house at night). The test scripts put a
kept copy of the autosave container in place before each start and the player's own back afterwards.

The sampling profiler (`LEONIDA_PROFILE`) must not be given the title's own threads
(`LEONIDA_PROFILE_ONLY=leonida-image`): walking one of their stacks faulted in `ntdll` 23 s into the window and
took the process down (log s3-prof). The timeline's sampler does not walk them and is safe.

## The card's compute passes, aliased views and batch passes (2026-11-28)

The same PC (RTX 4060 Ti, Series X profile, 2560x1440), every start from one copy of the autosave (a deck at the
waterside, in daylight), standing. Three things first, about measuring there:

- **The idle camera goes round three views of about 24 s each** (8,500, 12,500 and 14,700 draws; 71, 79 and 81 to
  90 ms a frame), and the same view costs a few ms more or less at another time. An A/B in turns of 15 s is as long
  as the views and measures them, not the switch (residual 3 to 4 ms a phase). Turns of 3.5 to 4 s, the phases
  cut into stretches wherever the draw count jumps, the first phase of a stretch left out and a level fitted for
  every stretch, give a switch's effect to +-0.5 ms in seven minutes.
- **A program left out says what it costs** (`LEONIDA_AB=<s>,LEONIDA_SKIP_TRY_A` with `LEONIDA_SKIP_TRY_A=<hashes>`).
  For the heavy compute programs the lines of a timing run are what they cost in the frame: left out,
  `CS_ProjDecalReduceClassification_Scalarized` makes the frame 2.3 ms shorter (+-0.55; its line 2.9 to 3.1 ms),
  the two `CS_ShadowRenderTiledCascades` programs 2.9 ms (+-0.56; their lines 2.5 together, and which of the two
  has the larger line changes from run to run: only their sum means something), `CS_ProjDecalResolve_Vectorized`
  with `CS_GenerateShadowMasks_HairOnly` 0.45 ms (+-0.57; lines 1.0).
- Phase medians pile up at 71.4 ms in the lightest view (15 of 45 phases of one run, to the tenth of a ms), and go
  below it with enough taken off the card (65 ms). What holds frames there is not found; means were fitted too.

### The form each heavy compute program gets on 32-lane waves, and what the forms cost

`LEONIDA_WAVE_WHY=1` on the 24 heaviest compute programs of a timing run, each translated alone into a scratch
cache. Seventeen translate as they are (`CS_FinalGather_*`, `CS_Gather_Classified_*`, `CS_UpdateParticles`,
`CS_ScreenProbes_*`, `CS_GenerateShadowMasks_Checkerboard_NoHair`, `CS_GenerateMaterialMasks`,
`CS_ReconstructRaymarchClassifiedFull`, `CS_BatchCullInitArgs` and others), `CS_TAA2_Resolve` as it is with the
entry ticket of an 8 x 8 group. The others, ms a frame in timing runs (two runs where two numbers stand):

| program | form the chain gives | ms | paired (`LEONIDA_PAIRED_FOR`) | wave-wide (`LEONIDA_WAVE_WIDE_FOR`) |
|---|---|---|---|---|
| `CS_ProjDecalReduceClassification_Scalarized` 1f1fbd86759b3da2 | without skips, scalars followed (exact, group barriers: 134, most in loops) | 2.85, 3.14 | 0.03, under 0.02 | not tried: the first form takes it |
| `CS_ShadowRenderTiledCascadesCB` be6ce5d849d51127 and `...AlphaCB` 922e1d0ee1e19a85 | the same | 2.48, 2.56 together | 1.60, 1.17 together | not tried |
| `CS_ProjDecalResolve_Vectorized` c4183d3cbee28e8e | local waves (APPROXIMATE, any mask), without early-outs | 0.53, 0.53 | 0.09 | 36.8 (the frame 96 ms) |
| `CS_GenerateShadowMasks_HairOnly` 0a5ecfee6b79e13b | the same | 0.47, 0.53 | 0.24 | 0.87 |
| `CS_DrawBatch` f8f50ab85e7b10bb | without skips (exact) | 0.55 | refused (an LDS operation) | - |

So the forms differ by far more than a factor: the group barriers of the exact forms are what costs (3 ms for a
program the paired form runs in 0.03, 37 ms for one in the wave-wide form), and the paired form, which has none,
is the cheapest of all, cheaper than the approximate one. Whole frames: 70.5 to 71.7 ms in a timing run with the
chain's forms, 67.9 to 68.3 with all five paired, 67.0 with the first three. Left out in turns in a run with the
five paired, the classification is no longer anything of the frame (+1.0 ms +-0.5 when it is left out), the two
shadow programs 1.3 ms (+-0.5, from 2.9) and the last two 0.8 (+-0.6, from 0.45: no difference).

Every frame of the standard moves was saved with the five paired and with none (526 and 513 frames): the scene,
its shadows on the deck, the fence and the player look the same, and the frames the scan flags (4 and 2) are the
roof beam passing a camera that looks up, and two darker frames the run without has too. No projected decal is
in view there, so the decal programs' pictures are not compared: the launcher names the two shadow programs only.

`LEONIDA_LOOPS_WAVE=1` would change four of the 24 (1f1fbd86759b3da2, f8f50ab85e7b10bb, c4183d3cbee28e8e,
0a5ecfee6b79e13b: 28, 1, 52 and 1 loop branches taken by each host wave together). In a timing run with
it the frame is 71.2 ms (70.5 to 71.7 without) and the four lines are 2.87, 0.48, 0.53 and 0.47 ms (3.14, 0.56,
0.53 and 0.53 without): none of them is slower or faster for it.

### The copies behind aliased views

All 59 aliased views at this place are R9G9B9E5 views the title places in R32_UINT textures its shaders write
(eleven of 2560x1440, eight of 1280x720, a 2048x2048, cubes, four-level atlases). 86 copies a frame of 57 views,
390 MiB, each through a buffer: 3.6 ms of a timing run's frame, and with no copy made at all
(`LEONIDA_EXP_NO_ALIAS_COPY`, in turns) the frame is 3.1 ms shorter (+-1.3). Direct3D copies R32_UINT into
R9G9B9E5 texel for texel, so the buffer is not needed (`LEONIDA_ALIAS_DIRECT=1`, tested on the card): 1.9 to 2.0
ms in a timing run, and in turns of 4 s the frame 2.6 ms shorter (+-0.9). The launcher sets it.

Of the 86 copies 53 are a list's first read of the view with the source as the list found it, 21 follow a write
by the list and 11 another write in the same list; none is made twice in a list without a write between. But by
view the copies outnumber the writes: a 2048x2048 view is copied 4.1 times a frame for 0.9 writes of its source,
seven 2560x1440 views every frame for sources written every other frame, two more 2.0 and 1.9 times for 0.5.
About 150 of the 390 MiB a frame copy a source nothing wrote since the view's last copy, in another list or the
frame before. Two ways on: leave such a copy out at submission (the list's prelude can decide in submission
order, as it does for depth twins), or make no copy: an R32_UINT texture made with R9G9B9E5 among its castable
formats (`CreateCommittedResource3`) can be read through the view itself, which is all of the 1.9 ms left.

### Batch passes and direct dispatches

- The pass that clamps a batch of indirect records runs 1,150 times a frame (`LEONIDA_INDIRECT_BATCH=1`): 4.8 ms
  of a timing run's frame as a command of its own (4.1 us each). With `2` (a batch ends only at the title's waits
  and draws) in turns of 4 s the frame is 6.5 ms shorter (+-0.8) with 450 to 600 passes: about 10 us of the frame
  a pass. `2` is not usable (it clamps records before the title's culling passes write them). Without the pass's
  wait for every earlier shader write the frame is the same (+0.3 +-0.3 ms; waiting for its sources only -0.1): the
  wait is not what costs. `3` (a batch ends for an execution only if shaders recorded since its pass are bound to
  memory its record lies in) is exact by what the list declares and takes off 0.8 ms (+-0.5): the passes stay at
  1,020 to 1,170, as most records lie in memory some storing shader of the list has bound (root views reach to
  the end of their buffer). What a dispatch writes has to be known per dispatch, not per list, to get further.
- `4` keeps it per dispatch: a batch stays open past a storing program only if every buffer store of its code goes
  through a view its bind declares (a table entry of unordered access at a fixed place, the first of its range, or
  a root view; resource tracking on a thread of its own, once a shader), and a record is clamped again behind a
  program bound to its bytes since the pass. Standing at the waterside with `LEONIDA_BATCH_WHY=1`: 900 to 1,000
  passes a frame (2: 235 to 240 by the same count), 780 to 840 of them for programs whose stores are not all
  declared: the meshlet culling passes (`CS_BatchCull*Meshlets*`, 13 to 26 a frame each, the five of them about
  100) and `CS_RaytracingResolve` (8 to 12 each), then a long tail; 24 to 30 records a frame clamped again behind a
  writer bound to them (one compute program's dispatch records for the next one's, 32 to 768 bytes). The culling
  passes and the resolve pick the views they store through from the heap by a number they compute (bindless):
  what they write is known at no bind, so they still end the batch: 4 takes 2.1 ms off where 2 takes 3.8 in the
  same run, walking 1.6 (+-1.6; table below). In every frame of the standard moves recorded with 1, 2 and 4 the
  scans found no sea fault, with 2 neither (table below): the pictures do not tell 4 from 2, and that 4 reads no
  record before its writer rests on what the binds declare. Not covered by them, as with 1: writes of another list
  of the same submission, a descriptor the title rewrites between recording and running; nor, with `LEONIDA_AB`
  switching between 2 and 4, the first batches of 4 after a phase of 2.
- 1,520 to 1,660 direct dispatches a frame: 46 % run one group, 19 % 2 to 16, 19 % 17 to 256, 12 % 257 to 4,096,
  4 % more; 82 % have a program that can store to buffers (which is what ends batches). The layer records no wait
  and no pass of its own around a direct dispatch. Their 16 to 17 ms of a timing run are not the many: the 830
  under 5 us each (463 of them `CS_ComputeImposterTransform`, 0.5 us each) are 0.9 ms together, 15 kinds over 200
  us each are 6.6 ms and 42 kinds of 50 to 200 us 4.2 ms: the screen-sized passes.
