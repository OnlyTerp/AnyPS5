### What

- Upload only the changed tile blocks of single-unit tiled storage images (`!blockUnits`, such as unaligned surfaces) when the host write watch or comparison identifies a subset of modified 64 KiB blocks (`StorageTexture::stampedRuns` + `uploadWindows`), instead of detiling and copying the entire surface.
- Upload only the changed row spans of single-pass linear storage images (`TextureTileMode::kLinear`) when a subset of 64 KiB blocks changed (`DetileOptions::rowBegin` / `rowEnd`), publishing unit shadows only across the modified byte runs.
- Mark pending only the tracked units a draw actually renders into (`StorageTexture::MarkDirtyRegion` / `regionUnits`), intersected across the render area, scissor rectangle, and triangle viewport bounds (`targetWrite` in `Draw.cpp`), rather than marking every unit of the resident target surface dirty on every draw.
- Add `APS5_NO_PARTIAL_IMAGE_UNITS` (documented in `docs/dev/ENVIRONMENT.md`) to fall back to whole-unit uploads and whole-slice dirty marking for A/B comparison.

### Tested

- Windows 11, NVIDIA GeForce RTX 5090:
  - `agc_driver_partial_image_units_tests` (**Green**, default): passes; a 64x64 scissored draw into a 256x256 `R32_SFLOAT` `TileR64KBX` color target (256 KiB across four 64 KiB units) marks 0 untouched units pending and retiles 1 unit (`65,536` bytes vs `262,144` bytes, **75% write-back reduction**); CPU 1-word stores to an unaligned 256x256 4 KiB tiled single-unit surface and to row 128 of a 256x256 linear single-unit surface each perform 1 partial upload of `65,536` bytes (vs `262,144` bytes, **75% upload reduction**).
  - `APS5_NO_PARTIAL_IMAGE_UNITS=1 agc_driver_partial_image_units_tests` (**Red**, optimization disabled): fails deterministically (`AGC graphics: partial image units: a 64x64 scissored draw marked untouched 64 KiB target units pending`).
  - `agc_driver_array_layer_target_tests`: passes.


#### Performance report

##### 1. PR onto `main` (`origin/main` `3fdd1f52e968` vs `arch/e-partial-image-upload-and-dirty-units-clean` `1167f0462ce8`)

Marking only scissored/rendered 64 KiB target units dirty (`StorageTexture::MarkDirtyRegion`) and uploading only modified tile/row spans eliminates all 48 false-positive 256 KiB GPU-to-CPU write-back flushes inside `Vulkan.DrawFromRecipe` (`7.757` -> `0.4` ms/frame), reducing `Driver.Packet` CPU time by **25.5%** (`14.175` -> `10.561` ms/frame), `queue worker 0x0` busy time from `84.088%` to `61.973%`, and `gpu_busy_ms_per_present` by **5.7%** (`5.026` -> `4.737` ms).

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`3fdd1f52e968` -> `1167f0462ce8`)**:
  - `frame_ms`: p50 `16.677` -> `16.682` ms | p90 `17.288` -> `16.831` ms | p99 `17.643` -> `18.766` ms | max `17.643` -> `18.766` ms
  - `gpu_busy_ms_per_present`: `5.026` -> `4.737` ms (`-5.8%`)
  - `Driver.Packet` (ms/frame): `14.175` -> `10.561` ms (`-25.5%`)
- **Attached Traces**:
  - Baseline (`3fdd1f52e968`): `U:/scratch/anyps5/traces/origin-main/aps5-trace-3fdd1f52e968-121052.json.gz` (summary: `U:/scratch/anyps5/traces/origin-main/aps5-summary.jsonl`)
  - Change (`1167f0462ce8`): `U:/scratch/anyps5/traces/e-partial-image-upload-and-dirty-units-clean/aps5-trace-1167f0462ce8-49412.json.gz` (summary: `U:/scratch/anyps5/traces/e-partial-image-upload-and-dirty-units-clean/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`3fdd1f52e968` vs `1167f0462ce8`)**:
```json
{"commit":"3fdd1f52e968","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":326.986},"presents":20,"frame_ms":{"p50":16.677,"p90":17.288,"p99":17.643,"max":17.643},"gpu_busy_ms_per_present":5.026,"worker_busy_pct":{"queue worker 0x20":0.935,"queue worker 0x21":0.831,"queue worker 0x0":85.718},"top_phases_ms_per_frame":{"Driver.Packet":14.175,"Driver.Packet.draw":12.722,"Driver.Draw":12.539,"Driver.Draw.draw_enqueued":8.822,"Vulkan.DrawFromRecipe":7.757,"Graphics.DrawWithRecipe":7.696,"Graphics.DrawWithRecipe.draw_and_resource_release":7.674,"Graphics.Draw":4.146},"trace_dropped":false,"trace":"aps5-trace-3fdd1f52e968-121052.json.gz"}
{"commit":"1167f0462ce8","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.611},"presents":20,"frame_ms":{"p50":16.682,"p90":16.831,"p99":18.766,"max":18.766},"gpu_busy_ms_per_present":4.737,"worker_busy_pct":{"queue worker 0x20":1.002,"queue worker 0x21":1.235,"queue worker 0x0":61.973},"top_phases_ms_per_frame":{"Driver.Packet":10.561,"Driver.Packet.draw":9.048,"Driver.Draw":8.861,"VideoOut.Flip":5.531,"Graphics.Draw":5.370,"Graphics.Draw.draw_and_resource_release":5.217,"VideoOut.Flip.vblank_wait":4.628,"Driver.Draw.draw_enqueued":4.442},"trace_dropped":false,"trace":"aps5-trace-1167f0462ce8-49412.json.gz"}
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
**`python tools/trace_digest.py` — With Change (`1167f0462ce8`, `aps5-trace-1167f0462ce8-49412.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.8 / p99 18.8 / max 18.8 ms, GPU busy 4.7 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 1167f0462ce8, dropped: no

```json
{"commit":"1167f0462ce8","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.611},"presents":20,"frame_ms":{"p50":16.682,"p90":16.831,"p99":18.766,"max":18.766},"gpu_busy_ms_per_present":4.737,"worker_busy_pct":{"queue worker 0x20":1.002,"queue worker 0x21":1.235,"queue worker 0x0":61.973},"top_phases_ms_per_frame":{"Driver.Packet":10.561,"Driver.Packet.draw":9.048,"Driver.Draw":8.861,"VideoOut.Flip":5.531,"Graphics.Draw":5.37,"Graphics.Draw.draw_and_resource_release":5.217,"VideoOut.Flip.vblank_wait":4.628,"Driver.Draw.draw_enqueued":4.442},"trace_dropped":false,"trace":"aps5-trace-1167f0462ce8-49412.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 9: 18.8 ms at 67 ms: Driver.WorkerWait (queue worker 0x20) 18.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 18.6, Driver.WorkerWait (queue worker 0x21) 18.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 18.6
- present 7: 16.8 ms at 33 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 22: 16.8 ms at 284 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 19: 16.8 ms at 234 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 16: 16.8 ms at 183 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.7 (94 %) | 0.8 | 0.2 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x21 | 0.1 (1 %) | 15.7 (94 %) | 0.8 | 0.2 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x0 | 5.1 (31 %) | 5.7 (34 %) | 5.9 | 21.0 | Driver.Packet 5.1, Driver.EndSubmission 0.0 |
| draw committer | 1.4 (8 %) | 0.1 (0 %) | 15.2 | 12.8 | Graphics.Draw 0.7, Vulkan.DrawFromRecipe 0.4 |
| presenter | 0.7 (4 %) | 5.1 (31 %) | 10.9 | 0.1 | VideoOut.Flip 5.5, Driver.Present 0.1 |
| gpu | 4.7 (28 %) | 0.0 (0 %) | 12.0 | 0.0 | batch 4.7 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 37.1, Driver.WorkerWait.idle_and_completion_poll 37.1, VideoOut.Flip 5.5, Driver.Packet 5.3, batch 4.7, Driver.Packet.draw 4.7, VideoOut.Flip.vblank_wait 4.6, Driver.Draw 4.6, Driver.Draw.draw_enqueued 1.9, Graphics.Draw 1.5

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 18.6 ms at 68 ms, present 9
- Driver.WorkerWait (queue worker 0x21) 18.6 ms at 68 ms, present 9
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 302 ms, present 23
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 285 ms, present 22
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 235 ms, present 19
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 285 ms, present 22
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 185 ms, present 16
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 302 ms, present 23
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 235 ms, present 19
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 118 ms, present 12

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 216: 0.8 ms at 254 ms, present 20: GPU no ranges; recorded by no CPU slices
- batch 23: 0.8 ms at 24 ms, present 6: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.1, Graphics.Draw.draw_and_resource_release (draw committer) 0.1, Recorder.Submit (draw committer) 0.1, Vulkan.DrawFromRecipe (draw committer) 0.0
- batch 63: 0.8 ms at 72 ms, present 9: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.1, Graphics.Draw.draw_and_resource_release (draw committer) 0.1, Recorder.Submit (draw committer) 0.0
- batch 165: 0.7 ms at 191 ms, present 16: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.2, Graphics.Draw.draw_and_resource_release (draw committer) 0.1, Vulkan.DrawFromRecipe (draw committer) 0.1, Graphics.DrawWithRecipe (draw committer) 0.1
- batch 106: 0.7 ms at 123 ms, present 12: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.1, Graphics.Draw.draw_and_resource_release (draw committer) 0.1, Vulkan.DrawFromRecipe (draw committer) 0.0, Graphics.DrawWithRecipe (draw committer) 0.0
````
**`python tools/trace_digest.py` — Interval Summary (`3fdd1f52e968` vs `1167f0462ce8`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
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
# With Change (1167f0462ce8)
**3 intervals**, 0.0 to 3.0 s, 181 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 1167f0462ce8

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.9 | 16.9 | 16.9 | 4.7 | 55 % | Driver.Packet 9.4 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 17.3 | 17.3 | 4.7 | 55 % | Driver.Packet 9.3 |
| 3 | 2-3 | 60 | 16.7 | 17.0 | 17.2 | 17.2 | 4.8 | 70 % | Driver.Packet 11.8 |

```
p99 ms  @@@  0-17.3
gpu ms  @@@  0-4.8
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
