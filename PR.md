### What
Bound host memory growth across the AGC driver's sampled-texture snapshots, shader registration prepare thread stacks, dispatch cache, and guest memory page-state and write-tracking tables:

- **Sampled-texture snapshot digests and large tracked surface release (`ShaderResources`, `GuestMemory`)**: Replace full host byte copies of sampled-texture snapshots under `APS5_SAMPLED_DIGEST=1` with 64-bit `Digest` / `DigestCommitted` hashes per committed run, report retained host snapshot bytes in `TextureCacheUsage::snapshotHostBytes`, and release host snapshot vectors larger than `256 KiB` after upload when write-tracking stamps or untracked hints cover the surface (`!SnapshotDigests() && (entry.generation != 0 || entry.untracked[0] != 0)`).
- **Reserved shader prepare thread stacks (`HostThread::StartDetached`, `ShaderRegistry`)**: Start detached host threads in `RegistrationPreparePool` with `STACK_SIZE_PARAM_IS_A_RESERVATION` on Windows so each 64 MiB stack reserves its address range and commits on demand (~20 KiB at thread start instead of 64 MiB upfront per worker).
- **Byte-bounded dispatch cache and hash-only differing history (`DispatchEntries`, `PriorValueSets`, `Driver`)**: Bound `DispatchEntries` by both entry count and variant capacity bytes (default `physical / 64` or `APS5_DISPATCH_CACHE_BYTES`), reserve `runs` and `words` to exact size before population, evict least-recently-used entries (and older variants of the active entry when needed) to stay within budget, and store 64-bit FNV-1a hashes instead of full run/word vector copies in `PriorValueSets`.
- **Demand-committed guest page-state table and per-leaf write-tracking tables on Windows (`TouchedTable`, `GuestMemory`)**: Back `PageSpan` on Windows with `TouchedTable<std::uint8_t>` (`MEM_RESERVE` + 64 KiB page commit on first store, lock-free untouched zero loads/run-scans/clears) and unify `WriteTracker`'s `blocks`, `cpuBlocks`, `coarseBlocks`, and Windows `hintLeaves` into demand-allocated 4 GiB leaves (`2^16` blocks per leaf).

Depends on: none

### Tested
- Built and ran `host_thread_stack`, `agc_touched_table`, `agc_write_tracking`, `agc_driver_dispatch_cache_budget`, `agc_driver_image_table_cache_bounds`, `agc_driver_partially_resident_snapshot`, `agc_driver_large_tracked_snapshot`, `agc_driver_sampled_cache_budget`, `agc_driver_sampled_snapshot_copies`, and `agc_driver_sampled_snapshot_digests` on Windows (`10/10` passed):
  - `host_thread_stack`: `a host thread with a 64 MiB stack committed 20 KiB at start and 63 MiB after touching it` (`0.06s`).
  - `agc_touched_table`: `commit charge: 64 scattered regions of 16 pages in a 250 MiB table commit 4096 KiB (system: 4096 KiB)`, `equivalence: a reserved byte table matches a committed array over 200000 loads, stores, run scans, clears and range queries (1728 of 2624 KiB committed)`, `concurrency: 8 threads writing first into the same 32 table pages commit each once` (`4.13s`).
  - `agc_write_tracking`: `stamping 16 scattered regions grew the stamp tables by 8256 KiB to 8256 KiB`, `400 stamps and reads over 3 areas, around a leaf boundary and at the arena's last block, match flat stamps`, `a guest memory table of 250 MiB has 2048 KiB committed after 32 scattered regions` (`1.61s`).
  - `agc_driver_dispatch_cache_budget`: `captures of 64 MiB kept the dispatch cache under its 256 MiB budget and still hit` (`1.20s`).
  - `agc_driver_sampled_snapshot_copies` & `agc_driver_sampled_snapshot_digests`: `8 cached snapshots of 4 MiB surfaces (copies) grew the heap by 32804 KiB, 32768 KiB of them host snapshot copies` vs `8 cached snapshots of 4 MiB surfaces (digests) grew the heap by 36 KiB, 0 KiB of them host snapshot copies`.
  - `agc_driver_large_tracked_snapshot` & `agc_driver_partially_resident_snapshot`: `partially resident snapshot tests passed` (`0.85s` / `0.84s`).
- Verified `python tools/check_conventions.py` (`All conventions checks passed.`) and `python tools/check_test_registration.py build` (`0 built and not run as recorded, none new`).


#### Performance report

##### 1. PR onto `main` (`origin/main` `3fdd1f52e968` vs `arch/b-sampled-snapshot-and-stacks-clean` `1df51dfd63ef`)

Demand-committed `TouchedTable` guest page-state/write-tracking leaves, reserved 64 MiB worker stacks (`STACK_SIZE_PARAM_IS_A_RESERVATION`), byte-bounded `DispatchEntries`, and sampled-texture snapshot digests/large-surface release reduce `Driver.Packet` from `14.175` to `13.423` ms/frame (**-5.3%**) and `p99` from `17.643` to `16.921` ms.

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`3fdd1f52e968` -> `1df51dfd63ef`)**:
  - `frame_ms`: p50 `16.677` -> `16.684` ms | p90 `17.288` -> `16.816` ms | p99 `17.643` -> `16.921` ms | max `17.643` -> `16.921` ms
  - `gpu_busy_ms_per_present`: `5.026` -> `6.423` ms (`27.8%`)
  - `Driver.Packet` (ms/frame): `14.175` -> `13.423` ms (`-5.3%`)
- **Attached Traces**:
  - Baseline (`3fdd1f52e968`): `U:/scratch/anyps5/traces/origin-main/aps5-trace-3fdd1f52e968-121052.json.gz` (summary: `U:/scratch/anyps5/traces/origin-main/aps5-summary.jsonl`)
  - Change (`1df51dfd63ef`): `U:/scratch/anyps5/traces/b-sampled-snapshot-and-stacks-clean/aps5-trace-1df51dfd63ef-42180.json.gz` (summary: `U:/scratch/anyps5/traces/b-sampled-snapshot-and-stacks-clean/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`3fdd1f52e968` vs `1df51dfd63ef`)**:
```json
{"commit":"3fdd1f52e968","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":326.986},"presents":20,"frame_ms":{"p50":16.677,"p90":17.288,"p99":17.643,"max":17.643},"gpu_busy_ms_per_present":5.026,"worker_busy_pct":{"queue worker 0x20":0.935,"queue worker 0x21":0.831,"queue worker 0x0":85.718},"top_phases_ms_per_frame":{"Driver.Packet":14.175,"Driver.Packet.draw":12.722,"Driver.Draw":12.539,"Driver.Draw.draw_enqueued":8.822,"Vulkan.DrawFromRecipe":7.757,"Graphics.DrawWithRecipe":7.696,"Graphics.DrawWithRecipe.draw_and_resource_release":7.674,"Graphics.Draw":4.146},"trace_dropped":false,"trace":"aps5-trace-3fdd1f52e968-121052.json.gz"}
{"commit":"1df51dfd63ef","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":330.043},"presents":20,"frame_ms":{"p50":16.684,"p90":16.816,"p99":16.921,"max":16.921},"gpu_busy_ms_per_present":6.423,"worker_busy_pct":{"queue worker 0x21":0.885,"queue worker 0x20":0.802,"queue worker 0x0":80.361},"top_phases_ms_per_frame":{"Driver.Packet":13.423,"Driver.Packet.draw":11.939,"Driver.Draw":11.762,"Driver.Draw.draw_enqueued":8.268,"Vulkan.DrawFromRecipe":7.365,"Graphics.DrawWithRecipe":7.309,"Graphics.DrawWithRecipe.draw_and_resource_release":7.288,"Graphics.Draw":4.060},"trace_dropped":false,"trace":"aps5-trace-1df51dfd63ef-42180.json.gz"}
```
**`python tools/trace_digest.py` — Baseline (`3fdd1f52e968`, `aps5-trace-3fdd1f52e968-121052.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 17.3 / p99 17.6 / max 17.6 ms, GPU busy 5.0 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 3fdd1f52e968, dropped: no

```json
{"commit":"3fdd1f52e968","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":326.986},"presents":20,"frame_ms":{"p50":16.677,"p90":17.288,"p99":17.643,"max":17.643},"gpu_busy_ms_per_present":5.026,"worker_busy_pct":{"queue worker 0x20":0.935,"queue worker 0x21":0.831,"queue worker 0x0":85.718},"top_phases_ms_per_frame":{"Driver.Packet":14.175,"Driver.Packet.draw":12.722,"Driver.Draw":12.539,"Driver.Draw.draw_enqueued":8.822,"Vulkan.DrawFromRecipe":7.757,"Graphics.DrawWithRecipe":7.696,"Graphics.DrawWithRecipe.draw_and_resource_release":7.674,"Graphics.Draw":4.146},"trace_dropped":false,"trace":"aps5-trace-3fdd1f52e968-121052.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 9: 17.6 ms at 67 ms: Driver.WorkerWait (queue worker 0x20) 17.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 17.5, Driver.WorkerWait (queue worker 0x21) 17.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 17.5
- present 7: 17.3 ms at 33 ms: Driver.WorkerWait (queue worker 0x20) 17.2, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 17.2, Driver.WorkerWait (queue worker 0x21) 17.1, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 17.1
- present 5: 17.2 ms at 0 ms: Driver.WorkerWait (queue worker 0x20) 15.9, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 15.9, Driver.WorkerWait (queue worker 0x21) 15.9, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 15.9
- present 21: 16.9 ms at 260 ms: Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8, Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7
- present 17: 16.9 ms at 193 ms: Driver.WorkerWait (queue worker 0x20) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.8, Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.4 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x21 | 0.1 (0 %) | 15.4 (94 %) | 0.9 | 0.1 | Driver.EndSubmission 0.1, Driver.Packet 0.0 |
| queue worker 0x0 | 8.5 (52 %) | 2.0 (12 %) | 5.9 | 21.3 | Driver.Packet 8.5 |
| draw committer | 7.0 (43 %) | 0.0 (0 %) | 9.3 | 10.1 | Vulkan.DrawFromRecipe 6.5, Graphics.Draw 0.4 |
| presenter | 0.8 (5 %) | 1.0 (6 %) | 14.6 | 0.0 | VideoOut.Flip 1.7, Driver.Present 0.0 |
| gpu | 5.0 (31 %) | 0.0 (0 %) | 11.3 | 0.0 | batch 5.0 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 32.7, Driver.WorkerWait.idle_and_completion_poll 32.7, Driver.Packet 8.6, Driver.Packet.draw 8.1, Driver.Draw 8.0, Driver.Draw.draw_enqueued 7.0, Vulkan.DrawFromRecipe 6.5, Graphics.DrawWithRecipe 6.5, Graphics.DrawWithRecipe.draw_and_resource_release 6.5, batch 5.0

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 17.4 ms at 68 ms, present 9
- Driver.WorkerWait (queue worker 0x21) 17.4 ms at 68 ms, present 9
- Driver.WorkerWait (queue worker 0x20) 17.2 ms at 1 ms, present 5
- Driver.WorkerWait (queue worker 0x21) 17.2 ms at 1 ms, present 5
- Driver.WorkerWait (queue worker 0x21) 16.8 ms at 261 ms, present 21
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 261 ms, present 21
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 228 ms, present 19
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 34 ms, present 7
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 195 ms, present 17
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 195 ms, present 17

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 883: 0.8 ms at 240 ms, present 19: GPU no ranges; recorded by Recorder.Submit (draw committer) 0.0, Graphics.Draw (draw committer) 0.0, Graphics.Draw.draw_and_resource_release (draw committer) 0.0
- batch 1174: 0.8 ms at 321 ms, present 24: GPU no ranges; recorded by no CPU slices
- batch 707: 0.7 ms at 190 ms, present 16: GPU no ranges; recorded by Recorder.Submit (draw committer) 0.0
- batch 410: 0.6 ms at 111 ms, present 11: GPU no ranges; recorded by no CPU slices
- batch 468: 0.5 ms at 126 ms, present 12: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.0, Graphics.Draw.draw_and_resource_release (draw committer) 0.0, Recorder.Submit (draw committer) 0.0
````
**`python tools/trace_digest.py` — With Change (`1df51dfd63ef`, `aps5-trace-1df51dfd63ef-42180.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.8 / p99 16.9 / max 16.9 ms, GPU busy 6.4 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 1df51dfd63ef, dropped: no

```json
{"commit":"1df51dfd63ef","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":330.043},"presents":20,"frame_ms":{"p50":16.684,"p90":16.816,"p99":16.921,"max":16.921},"gpu_busy_ms_per_present":6.423,"worker_busy_pct":{"queue worker 0x21":0.885,"queue worker 0x20":0.802,"queue worker 0x0":80.361},"top_phases_ms_per_frame":{"Driver.Packet":13.423,"Driver.Packet.draw":11.939,"Driver.Draw":11.762,"Driver.Draw.draw_enqueued":8.268,"Vulkan.DrawFromRecipe":7.365,"Graphics.DrawWithRecipe":7.309,"Graphics.DrawWithRecipe.draw_and_resource_release":7.288,"Graphics.Draw":4.06},"trace_dropped":false,"trace":"aps5-trace-1df51dfd63ef-42180.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 8: 16.9 ms at 46 ms: Driver.WorkerWait (queue worker 0x20) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.8, Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8
- present 15: 16.8 ms at 163 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7
- present 19: 16.8 ms at 230 ms: Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 23: 16.8 ms at 297 ms: Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7, Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7
- present 11: 16.8 ms at 96 ms: Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6, Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x21 | 0.1 (1 %) | 15.6 (94 %) | 0.9 | 0.1 | Driver.EndSubmission 0.1, Driver.Packet 0.1 |
| queue worker 0x20 | 0.1 (1 %) | 15.6 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.0 |
| queue worker 0x0 | 7.9 (48 %) | 2.8 (17 %) | 5.9 | 21.0 | Driver.Packet 7.9 |
| draw committer | 6.5 (39 %) | 0.0 (0 %) | 10.0 | 10.0 | Vulkan.DrawFromRecipe 6.2, Graphics.Draw 0.2 |
| presenter | 0.7 (5 %) | 1.9 (12 %) | 13.8 | 0.1 | VideoOut.Flip 2.5, Driver.Present 0.0 |
| gpu | 6.2 (37 %) | 0.0 (0 %) | 10.3 | 0.0 | batch 6.2 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 33.8, Driver.WorkerWait.idle_and_completion_poll 33.8, Driver.Packet 8.0, Driver.Packet.draw 7.4, Driver.Draw 7.3, Driver.Draw.draw_enqueued 6.3, batch 6.2, Vulkan.DrawFromRecipe 6.2, Graphics.DrawWithRecipe 6.2, Graphics.DrawWithRecipe.draw_and_resource_release 6.2

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 16.9 ms at 48 ms, present 8
- Driver.WorkerWait (queue worker 0x21) 16.8 ms at 48 ms, present 8
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 264 ms, present 21
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 298 ms, present 23
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 98 ms, present 11
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 264 ms, present 21
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 198 ms, present 17
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 298 ms, present 23
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 198 ms, present 17
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 98 ms, present 11

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 1121: 1.0 ms at 308 ms, present 23: GPU no ranges; recorded by no CPU slices
- batch 826: 1.0 ms at 225 ms, present 18: GPU no ranges; recorded by no CPU slices
- batch 1180: 0.9 ms at 324 ms, present 24: GPU no ranges; recorded by no CPU slices
- batch 884: 0.9 ms at 241 ms, present 19: GPU no ranges; recorded by no CPU slices
- batch 231: 0.9 ms at 58 ms, present 8: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — Interval Summary (`3fdd1f52e968` vs `1df51dfd63ef`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
````markdown
# Baseline (3fdd1f52e968)
**3 intervals**, 0.0 to 3.0 s, 137 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 3fdd1f52e968

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 55 | 17.6 | 21.2 | 29.4 | 29.4 | 5.3 | 90 % | Driver.Packet 16.7 |
| 2 | 1-2 | 33 | 28.7 | 38.7 | 65.2 | 65.2 | 5.5 | 92 % | Driver.Packet 28.4 |
| 3 | 2-3 | 49 | 20.4 | 22.2 | 25.1 | 25.1 | 5.3 | 91 % | Driver.Packet 18.9 |

```
p99 ms  =@-  0-65.2
gpu ms  @@@  0-5.5
```
# With Change (1df51dfd63ef)
**3 intervals**, 0.0 to 3.0 s, 180 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 1df51dfd63ef

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.9 | 19.4 | 19.4 | 6.0 | 79 % | Driver.Packet 13.4 |
| 2 | 1-2 | 60 | 16.7 | 17.0 | 20.9 | 20.9 | 5.9 | 73 % | Driver.Packet 12.3 |
| 3 | 2-3 | 59 | 16.9 | 18.7 | 20.1 | 20.1 | 5.6 | 87 % | Driver.Packet 15.1 |

```
p99 ms  %@@  0-20.9
gpu ms  @@%  0-6.0
```
````

---

##### 2. Stacked Lane B (`arch/b-pipeline-memory-gate-clean` `a01477c5e2dd` vs `arch/stack-b1-plus-b2-clean` `1f016ef8a7d4`)

When stacked on top of Lane B.1 (`arch/b-pipeline-memory-gate-clean`), the combined B.1 + B.2 stack (`arch/stack-b1-plus-b2-clean`, commit `1f016ef8a7d4`) maintains locked 60 FPS (`p50: 16.617` ms, `p90: 16.942` ms).

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`a01477c5e2dd` -> `1f016ef8a7d4`)**:
  - `frame_ms`: p50 `16.681` -> `16.617` ms | p90 `16.959` -> `16.942` ms | p99 `16.959` -> `17.657` ms | max `16.959` -> `17.657` ms
  - `gpu_busy_ms_per_present`: `5.78` -> `6.011` ms (`4.0%`)
  - `Driver.Packet` (ms/frame): `12.942` -> `14.257` ms (`10.2%`)
- **Attached Traces**:
  - Baseline (`a01477c5e2dd`): `U:/scratch/anyps5/traces/b-pipeline-memory-gate-clean/aps5-trace-a01477c5e2dd-123612.json.gz` (summary: `U:/scratch/anyps5/traces/b-pipeline-memory-gate-clean/aps5-summary.jsonl`)
  - Change (`1f016ef8a7d4`): `U:/scratch/anyps5/traces/stack-b1-plus-b2-clean/aps5-trace-1f016ef8a7d4-62628.json.gz` (summary: `U:/scratch/anyps5/traces/stack-b1-plus-b2-clean/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`a01477c5e2dd` vs `1f016ef8a7d4`)**:
```json
{"commit":"a01477c5e2dd","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":330.562},"presents":20,"frame_ms":{"p50":16.681,"p90":16.959,"p99":16.959,"max":16.959},"gpu_busy_ms_per_present":5.780,"worker_busy_pct":{"queue worker 0x20":0.752,"queue worker 0x21":0.920,"queue worker 0x0":77.308},"top_phases_ms_per_frame":{"Driver.Packet":12.942,"Driver.Packet.draw":11.507,"Driver.Draw":11.317,"Driver.Draw.draw_enqueued":8.019,"Vulkan.DrawFromRecipe":7.022,"Graphics.DrawWithRecipe":6.965,"Graphics.DrawWithRecipe.draw_and_resource_release":6.944,"Graphics.Draw":3.936},"trace_dropped":false,"trace":"aps5-trace-a01477c5e2dd-123612.json.gz"}
{"commit":"1f016ef8a7d4","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":328.138},"presents":20,"frame_ms":{"p50":16.617,"p90":16.942,"p99":17.657,"max":17.657},"gpu_busy_ms_per_present":6.011,"worker_busy_pct":{"queue worker 0x20":0.901,"queue worker 0x21":0.941,"queue worker 0x0":85.819},"top_phases_ms_per_frame":{"Driver.Packet":14.257,"Driver.Packet.draw":12.699,"Driver.Draw":12.516,"Driver.Draw.draw_enqueued":8.793,"Vulkan.DrawFromRecipe":7.686,"Graphics.DrawWithRecipe":7.625,"Graphics.DrawWithRecipe.draw_and_resource_release":7.603,"Graphics.Draw":4.284},"trace_dropped":false,"trace":"aps5-trace-1f016ef8a7d4-62628.json.gz"}
```
**`python tools/trace_digest.py` — Baseline (`a01477c5e2dd`, `aps5-trace-a01477c5e2dd-123612.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 17.0 / p99 17.0 / max 17.0 ms, GPU busy 5.8 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit a01477c5e2dd, dropped: no

```json
{"commit":"a01477c5e2dd","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":330.562},"presents":20,"frame_ms":{"p50":16.681,"p90":16.959,"p99":16.959,"max":16.959},"gpu_busy_ms_per_present":5.78,"worker_busy_pct":{"queue worker 0x20":0.752,"queue worker 0x21":0.92,"queue worker 0x0":77.308},"top_phases_ms_per_frame":{"Driver.Packet":12.942,"Driver.Packet.draw":11.507,"Driver.Draw":11.317,"Driver.Draw.draw_enqueued":8.019,"Vulkan.DrawFromRecipe":7.022,"Graphics.DrawWithRecipe":6.965,"Graphics.DrawWithRecipe.draw_and_resource_release":6.944,"Graphics.Draw":3.936},"trace_dropped":false,"trace":"aps5-trace-a01477c5e2dd-123612.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 24: 17.0 ms at 314 ms: Driver.Packet (queue worker 0x0) 6.7, Driver.Packet.draw (queue worker 0x0) 6.3, Driver.Draw (queue worker 0x0) 6.3, batch (gpu) 6.0
- present 16: 17.0 ms at 180 ms: Driver.WorkerWait (queue worker 0x20) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.8, Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8
- present 21: 16.9 ms at 264 ms: Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8, Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7
- present 12: 16.8 ms at 113 ms: Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7, Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7
- present 10: 16.8 ms at 80 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.6 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.0 |
| queue worker 0x21 | 0.1 (1 %) | 15.6 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x0 | 7.4 (44 %) | 3.2 (19 %) | 6.0 | 20.8 | Driver.Packet 7.4 |
| draw committer | 6.1 (37 %) | 0.0 (0 %) | 10.4 | 9.9 | Vulkan.DrawFromRecipe 5.8, Graphics.Draw 0.2 |
| presenter | 0.7 (4 %) | 2.4 (15 %) | 13.3 | 0.1 | VideoOut.Flip 3.0, Driver.Present 0.0 |
| gpu | 5.8 (35 %) | 0.0 (0 %) | 10.8 | 0.0 | batch 5.8 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 34.3, Driver.WorkerWait.idle_and_completion_poll 34.3, Driver.Packet 7.5, Driver.Packet.draw 7.0, Driver.Draw 6.9, Driver.Draw.draw_enqueued 6.1, Vulkan.DrawFromRecipe 5.8, Graphics.DrawWithRecipe 5.8, Graphics.DrawWithRecipe.draw_and_resource_release 5.8, batch 5.8

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 181 ms, present 16
- Driver.WorkerWait (queue worker 0x21) 16.8 ms at 181 ms, present 16
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 131 ms, present 13
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 132 ms, present 13
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 81 ms, present 10
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 265 ms, present 21
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 215 ms, present 18
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 248 ms, present 20
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 215 ms, present 18
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 265 ms, present 21

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 886: 1.0 ms at 243 ms, present 19: GPU no ranges; recorded by no CPU slices
- batch 1005: 0.9 ms at 276 ms, present 21: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.1, Graphics.Draw.draw_and_resource_release (draw committer) 0.1, Resources.Prepare (draw committer) 0.1, Recorder.Submit (draw committer) 0.0
- batch 946: 0.9 ms at 259 ms, present 20: GPU no ranges; recorded by Recorder.Submit (draw committer) 0.0
- batch 942: 0.9 ms at 257 ms, present 20: GPU no ranges; recorded by no CPU slices
- batch 467: 0.9 ms at 123 ms, present 12: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — With Change (`1f016ef8a7d4`, `aps5-trace-1f016ef8a7d4-62628.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.6 / p90 16.9 / p99 17.7 / max 17.7 ms, GPU busy 6.0 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 1f016ef8a7d4, dropped: no

```json
{"commit":"1f016ef8a7d4","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":328.138},"presents":20,"frame_ms":{"p50":16.617,"p90":16.942,"p99":17.657,"max":17.657},"gpu_busy_ms_per_present":6.011,"worker_busy_pct":{"queue worker 0x20":0.901,"queue worker 0x21":0.941,"queue worker 0x0":85.819},"top_phases_ms_per_frame":{"Driver.Packet":14.257,"Driver.Packet.draw":12.699,"Driver.Draw":12.516,"Driver.Draw.draw_enqueued":8.793,"Vulkan.DrawFromRecipe":7.686,"Graphics.DrawWithRecipe":7.625,"Graphics.DrawWithRecipe.draw_and_resource_release":7.603,"Graphics.Draw":4.284},"trace_dropped":false,"trace":"aps5-trace-1f016ef8a7d4-62628.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 15: 17.7 ms at 161 ms: Driver.WorkerWait (queue worker 0x20) 17.5, Driver.WorkerWait (queue worker 0x21) 17.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 17.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 17.5
- present 14: 16.9 ms at 144 ms: Driver.WorkerWait (queue worker 0x20) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.8, Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8
- present 24: 16.9 ms at 311 ms: Driver.Packet (queue worker 0x0) 8.0, Driver.Packet.draw (queue worker 0x0) 7.7, Driver.Draw (queue worker 0x0) 7.5, Driver.Draw.draw_enqueued (queue worker 0x0) 6.7
- present 20: 16.9 ms at 244 ms: Driver.WorkerWait (queue worker 0x20) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7
- present 17: 16.8 ms at 195 ms: Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7, Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.4 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x21 | 0.1 (1 %) | 15.4 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x0 | 8.7 (53 %) | 1.9 (11 %) | 5.9 | 21.1 | Driver.Packet 8.7 |
| draw committer | 7.0 (42 %) | 0.0 (0 %) | 9.4 | 10.3 | Vulkan.DrawFromRecipe 6.4, Graphics.Draw 0.4 |
| presenter | 0.8 (5 %) | 1.0 (6 %) | 14.6 | 0.1 | VideoOut.Flip 1.7, Driver.Present 0.1 |
| gpu | 6.0 (37 %) | 0.0 (0 %) | 10.4 | 0.0 | batch 6.0 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 32.7, Driver.WorkerWait.idle_and_completion_poll 32.7, Driver.Packet 8.8, Driver.Packet.draw 8.1, Driver.Draw 8.0, Driver.Draw.draw_enqueued 6.8, Vulkan.DrawFromRecipe 6.4, Graphics.DrawWithRecipe 6.4, Graphics.DrawWithRecipe.draw_and_resource_release 6.4, batch 6.0

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 17.5 ms at 163 ms, present 15
- Driver.WorkerWait (queue worker 0x21) 17.5 ms at 163 ms, present 15
- Driver.WorkerWait (queue worker 0x20) 16.9 ms at 146 ms, present 14
- Driver.WorkerWait (queue worker 0x21) 16.9 ms at 146 ms, present 14
- Driver.WorkerWait (queue worker 0x20) 16.9 ms at 246 ms, present 20
- Driver.WorkerWait (queue worker 0x21) 16.8 ms at 246 ms, present 20
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 112 ms, present 12
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 112 ms, present 12
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 196 ms, present 17
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 229 ms, present 19

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 1058: 1.1 ms at 289 ms, present 22: GPU no ranges; recorded by no CPU slices
- batch 703: 1.1 ms at 190 ms, present 16: GPU no ranges; recorded by no CPU slices
- batch 940: 1.0 ms at 256 ms, present 20: GPU no ranges; recorded by no CPU slices
- batch 880: 0.9 ms at 239 ms, present 19: GPU no ranges; recorded by GpuMutex.wait.draw (draw committer) 0.0
- batch 112: 0.9 ms at 26 ms, present 6: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.1, Graphics.Draw.draw_and_resource_release (draw committer) 0.1, Recorder.Submit (draw committer) 0.0
````
**`python tools/trace_digest.py` — Interval Summary (`a01477c5e2dd` vs `1f016ef8a7d4`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
````markdown
# Baseline (a01477c5e2dd)
**3 intervals**, 0.0 to 3.0 s, 180 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit a01477c5e2dd

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.9 | 18.0 | 18.0 | 5.9 | 75 % | Driver.Packet 12.7 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 17.3 | 17.3 | 5.6 | 73 % | Driver.Packet 12.3 |
| 3 | 2-3 | 59 | 17.0 | 18.2 | 19.1 | 19.1 | 5.5 | 87 % | Driver.Packet 15.1 |

```
p99 ms  %%@  0-19.1
gpu ms  @@%  0-5.9
```
# With Change (1f016ef8a7d4)
**3 intervals**, 0.0 to 3.0 s, 174 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 1f016ef8a7d4

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 60 | 16.7 | 17.0 | 22.1 | 22.1 | 6.1 | 82 % | Driver.Packet 13.9 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 19.1 | 19.1 | 6.1 | 80 % | Driver.Packet 13.5 |
| 3 | 2-3 | 54 | 18.8 | 20.4 | 22.7 | 22.7 | 5.7 | 89 % | Driver.Packet 17.0 |

```
p99 ms  @%@  0-22.7
gpu ms  @@%  0-6.1
```
````

### Checklist
- [x] Followed the project's [coding conventions](https://github.com/AnyPS5/AnyPS5/blob/main/CONVENTIONS.md) (`python3 tools/check_conventions.py`)
- [x] Linked the dependency PRs above (`Depends on: #...`), or wrote `Depends on: none` if this PR stands alone
- [x] Every new test executable in `CMakeLists.txt` has a matching `add_test(...)` (`python3 tools/check_test_registration.py <build-dir>`)
- [x] Filled in `### What` and `### Tested` above with concrete details (not placeholders)
- [x] Disclosed AI usage below (`- [x] Depends on: none
- [x] GPUs tested: NVIDIA GeForce RTX 5090 (driver 610.52, Windows 11)
- [x] AI-assisted: yes` or `AI-assisted: no`)

AI-assisted: yes
