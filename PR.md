### What

Bounds an address-based draw render-pass hazard write set by `GuestBufferMemory::WritableAddressRanges()` (gated by `APS5_NO_BOUNDED_ADDRESS_PASSES`), returns `VulkanDevice::readyContext()` by const reference across post-initialization packet paths instead of copying `Graphics::Context` by value, and maintains `Recorder` pending-write range snapshots incrementally through `PendingWriteRanges` without rebuilding the union on every addition or batch retirement.

### Tested

`agc_pending_write_ranges_tests`:
- Green: `pending write ranges merge on record, leave on retirement and rebuild only on changed sources` (20,000 random steps with 1 initial rebuild).

`agc_driver_recorder_tests --pending-write-snapshot-only`:
- Green: passes with incremental `PendingWriteRanges` publication.

`agc_driver_address_passes_tests`:
- Red (`APS5_NO_BOUNDED_ADDRESS_PASSES=1 --store-beside-reads`): fails deterministically (`address passes: a draw whose table grants no write over the pass's reads did not continue its pass: 1 address-based draws, 1 began a pass, 0 continued one`).
- Green (default): passes all cases including `--store-beside-reads` (`a draw storing through the table after a draw reading read-only memory: 1 address-based draws, 0 began a pass, 1 continued one`).


#### Performance report

##### 1. PR onto `main` (`origin/main` `3fdd1f52e968` vs `arch/c-command-stream-and-pass-continuation-clean` `58367f62f444`)

Bounding address-based draw render-pass hazard write sets by `GuestBufferMemory::WritableAddressRanges()`, returning `VulkanDevice::readyContext()` by const reference, and maintaining `PendingWriteRanges` incrementally reduces `Driver.Packet` CPU time by **10.3%** (`14.175` -> `12.714` ms/frame) and `p99` frame time by **3.8%** (`17.643` -> `16.969` ms).

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`3fdd1f52e968` -> `58367f62f444`)**:
  - `frame_ms`: p50 `16.677` -> `16.681` ms | p90 `17.288` -> `16.942` ms | p99 `17.643` -> `16.969` ms | max `17.643` -> `16.969` ms
  - `gpu_busy_ms_per_present`: `5.026` -> `5.515` ms (`9.7%`)
  - `Driver.Packet` (ms/frame): `14.175` -> `12.714` ms (`-10.3%`)
- **Attached Traces**:
  - Baseline (`3fdd1f52e968`): `U:/scratch/anyps5/traces/origin-main/aps5-trace-3fdd1f52e968-121052.json.gz` (summary: `U:/scratch/anyps5/traces/origin-main/aps5-summary.jsonl`)
  - Change (`58367f62f444`): `U:/scratch/anyps5/traces/c-command-stream-and-pass-continuation-clean/aps5-trace-58367f62f444-81116.json.gz` (summary: `U:/scratch/anyps5/traces/c-command-stream-and-pass-continuation-clean/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`3fdd1f52e968` vs `58367f62f444`)**:
```json
{"commit":"3fdd1f52e968","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":326.986},"presents":20,"frame_ms":{"p50":16.677,"p90":17.288,"p99":17.643,"max":17.643},"gpu_busy_ms_per_present":5.026,"worker_busy_pct":{"queue worker 0x20":0.935,"queue worker 0x21":0.831,"queue worker 0x0":85.718},"top_phases_ms_per_frame":{"Driver.Packet":14.175,"Driver.Packet.draw":12.722,"Driver.Draw":12.539,"Driver.Draw.draw_enqueued":8.822,"Vulkan.DrawFromRecipe":7.757,"Graphics.DrawWithRecipe":7.696,"Graphics.DrawWithRecipe.draw_and_resource_release":7.674,"Graphics.Draw":4.146},"trace_dropped":false,"trace":"aps5-trace-3fdd1f52e968-121052.json.gz"}
{"commit":"58367f62f444","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":329.221},"presents":20,"frame_ms":{"p50":16.681,"p90":16.942,"p99":16.969,"max":16.969},"gpu_busy_ms_per_present":5.515,"worker_busy_pct":{"queue worker 0x20":0.835,"queue worker 0x21":0.943,"queue worker 0x0":76.258},"top_phases_ms_per_frame":{"Driver.Packet":12.714,"Driver.Packet.draw":11.265,"Driver.Draw":11.078,"Driver.Draw.draw_enqueued":7.762,"Vulkan.DrawFromRecipe":6.849,"Graphics.DrawWithRecipe":6.805,"Graphics.DrawWithRecipe.draw_and_resource_release":6.784,"Graphics.Draw":3.860},"trace_dropped":false,"trace":"aps5-trace-58367f62f444-81116.json.gz"}
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
**`python tools/trace_digest.py` — With Change (`58367f62f444`, `aps5-trace-58367f62f444-81116.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.9 / p99 17.0 / max 17.0 ms, GPU busy 5.5 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 58367f62f444, dropped: no

```json
{"commit":"58367f62f444","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":329.221},"presents":20,"frame_ms":{"p50":16.681,"p90":16.942,"p99":16.969,"max":16.969},"gpu_busy_ms_per_present":5.515,"worker_busy_pct":{"queue worker 0x20":0.835,"queue worker 0x21":0.943,"queue worker 0x0":76.258},"top_phases_ms_per_frame":{"Driver.Packet":12.714,"Driver.Packet.draw":11.265,"Driver.Draw":11.078,"Driver.Draw.draw_enqueued":7.762,"Vulkan.DrawFromRecipe":6.849,"Graphics.DrawWithRecipe":6.805,"Graphics.DrawWithRecipe.draw_and_resource_release":6.784,"Graphics.Draw":3.86},"trace_dropped":false,"trace":"aps5-trace-58367f62f444-81116.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 15: 17.0 ms at 162 ms: Driver.WorkerWait (queue worker 0x20) 16.9, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.9, Driver.WorkerWait (queue worker 0x21) 16.9, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.9
- present 19: 16.9 ms at 229 ms: Driver.WorkerWait (queue worker 0x20) 16.9, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.9, Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8
- present 11: 16.9 ms at 96 ms: Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8, Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7
- present 13: 16.7 ms at 129 ms: Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 8: 16.7 ms at 46 ms: Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6, Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.5 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x21 | 0.1 (1 %) | 15.5 (94 %) | 0.9 | 0.1 | Driver.EndSubmission 0.1, Driver.Packet 0.1 |
| queue worker 0x0 | 7.2 (44 %) | 3.4 (21 %) | 5.8 | 20.2 | Driver.Packet 7.2 |
| draw committer | 6.2 (37 %) | 0.1 (0 %) | 10.2 | 9.2 | Vulkan.DrawFromRecipe 5.7, Graphics.DrawWithRecipe 0.2 |
| presenter | 0.7 (4 %) | 2.7 (17 %) | 13.0 | 0.1 | VideoOut.Flip 3.2, Driver.Present 0.0 |
| gpu | 5.3 (32 %) | 0.0 (0 %) | 11.2 | 0.0 | batch 5.3 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 34.4, Driver.WorkerWait.idle_and_completion_poll 34.4, Driver.Packet 7.4, Driver.Packet.draw 6.8, Driver.Draw 6.8, Driver.Draw.draw_enqueued 6.1, Vulkan.DrawFromRecipe 5.9, Graphics.DrawWithRecipe 5.9, Graphics.DrawWithRecipe.draw_and_resource_release 5.9, batch 5.3

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 17.0 ms at 164 ms, present 15
- Driver.WorkerWait (queue worker 0x21) 17.0 ms at 164 ms, present 15
- Driver.WorkerWait (queue worker 0x20) 17.0 ms at 230 ms, present 19
- Driver.WorkerWait (queue worker 0x21) 16.9 ms at 97 ms, present 11
- Driver.WorkerWait (queue worker 0x21) 16.8 ms at 230 ms, present 19
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 97 ms, present 11
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 280 ms, present 22
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 280 ms, present 22
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 30 ms, present 7
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 63 ms, present 9

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 769: 0.8 ms at 208 ms, present 17: GPU no ranges; recorded by Recorder.Submit (draw committer) 0.0
- batch 1005: 0.8 ms at 275 ms, present 21: GPU no ranges; recorded by GpuMutex.wait.draw (draw committer) 0.0
- batch 1065: 0.8 ms at 291 ms, present 22: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.1, Graphics.Draw.draw_and_resource_release (draw committer) 0.1, Recorder.Submit (draw committer) 0.0, Recorder.Reap (draw committer) 0.0
- batch 354: 0.8 ms at 91 ms, present 10: GPU no ranges; recorded by no CPU slices
- batch 528: 0.8 ms at 139 ms, present 13: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — Interval Summary (`3fdd1f52e968` vs `58367f62f444`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
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
# With Change (58367f62f444)
**3 intervals**, 0.0 to 3.0 s, 179 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 58367f62f444

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.9 | 20.0 | 20.0 | 5.6 | 75 % | Driver.Packet 12.5 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 17.0 | 17.0 | 5.5 | 73 % | Driver.Packet 12.4 |
| 3 | 2-3 | 58 | 17.4 | 18.6 | 20.9 | 20.9 | 5.2 | 86 % | Driver.Packet 15.2 |

```
p99 ms  @#@  0-20.9
gpu ms  @@%  0-5.6
```
````

### Checklist
- [x] Based on current `main`; no other open PR implements the same functions
- [x] One topic per PR; follow-ups go in a new PR
- [x] No comments in code except [technical debt](https://github.com/boykopovar/AnyPS5/blob/main/docs/dev/CONVENTIONS.md)
- [x] Unimplemented paths throw (`NotImplemented_nid_no_patch`); silent stubs are listed under Silent stubs in their library's [technical debt](https://github.com/boykopovar/AnyPS5/tree/main/docs/dev/debt) file
- [x] No notes, investigation `.md` files or images added to the repository (attach them to this PR)
- [x] New third-party code is a submodule built from source
- [x] Generic behaviour, not specific to one title
- [x] Depends on: none
- [x] Fixes: none
- [x] Unblocks: none
- [x] GPUs tested: NVIDIA GeForce RTX 5090 (Windows 11)
- [x] AI-assisted: yes
