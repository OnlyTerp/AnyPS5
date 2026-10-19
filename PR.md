Depends on: #6946, #6947, #7642

### What

- **Out-of-line `DontInline` table image functions & structural deduplication (`SpirvFunctionMatch`)**: Emit CPU table image mode switches into `DontInline` SPIR-V helper functions (`table_image_<resource>`) taking only referenced table access words and non-immediate operands, and deduplicate alpha-equivalent helper definitions (`SameSpirvFunctionBody`) across both table image helpers and `device_image_lookup`.
- **Out-of-line multi-word call result spilling (`SpirvModule::MoveLargeCallResultsToVariables`)**: Spill composite `OpFunctionCall` results of 8 or more scalar components into caller-side `OpVariable` slots in `Function` storage class so drivers do not allocate distinct live VGPR ranges per call site.
- **Access-site device-table mode switches with a 512-arm program budget (`InlineModeSwitchesFit`)**: Emit device-table mode switches inline at the access site when `laneCount == 1` and total runtime mode arms across the program's table and device-table accesses do not exceed `512`, falling back to shared `DontInline` `table_image_*` helper functions once the program exceeds that budget.
- **Hoisted per-arm swizzle decoding, store candidate extraction, level-zero unnormalized sampling & vector bitcast**: Cache decoded T# dword-3 swizzle selectors per basic block (`SpirvSwizzleSelectors`), extract and narrow store source components once per mode arm instead of four times, collapse identical level-zero branches on runtime unnormalized samplers into a single `OpImageSampleExplicitLod`, and emit a single 4-component `OpBitcast` for 32-bit sample/load result vectors.

### Tested

- `agc_spirv_function_match_tests`: Passed (35 ms; Red/Green verified by mutating `SameSpirvFunctionBody` in `SpirvFunctionMatch.cpp`)
- `agc_spirv_call_results_tests`: Passed (37 ms)
- `agc_shader_memory_tests --bindless`: Passed (1,041 ms)
- `agc_shader_memory_tests --device-image-tables`: Passed (882 ms; 2 vs 3 inline device-table samples add `459 <= 2,450` SPIR-V words; 64 samples exceed the 512-arm budget and share 1 `DontInline` helper called 64 times; mesh program with 2,000-entry table: subgroup 64 is `8,642` words CPU / `11,192` words device, subgroup 32 is `13,702` words CPU / `15,880` words device)
- `agc_shader_memory_tests`: Passed (2,729 ms)
- `agc_driver_bindless_images_tests`: Passed on RTX 5090 (624 ms)
- `agc_driver_image_table_tests`: Passed on RTX 5090 (1,648 ms)
- `agc_driver_device_image_table_tests` & `--village-bench`: Passed on RTX 5090 (6,961 ms / 25,708 ms)
- `python tools/check_conventions.py --base refs/remotes/pr/7642 --head HEAD`: 0 findings
- `python tools/check_test_registration.py`: All test files registered


#### Performance report

##### 1. PR onto `main` (`origin/main` `3fdd1f52e968` vs `arch/a-device-image-code-size-clean` `c23ffbbc59b6` — Device-Image Path `APS5_GPU_PATH=1`)

Out-of-line `DontInline` table image helpers, structural helper deduplication (`SameSpirvFunctionBody`), and caller-side `OpVariable` spilling for >=8-component composite returns reduce `Driver.Packet` CPU time by **17.6%** (`15.349` -> `12.649` ms/frame) and `p99` frame time by **9.5%** (`18.740` -> `16.953` ms) under `APS5_GPU_PATH=1`.

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`3fdd1f52e968` -> `c23ffbbc59b6`)**:
  - `frame_ms`: p50 `16.898` -> `16.681` ms | p90 `18.484` -> `16.886` ms | p99 `18.74` -> `16.953` ms | max `18.74` -> `16.953` ms
  - `gpu_busy_ms_per_present`: `5.883` -> `6.065` ms (`3.1%`)
  - `Driver.Packet` (ms/frame): `15.349` -> `12.649` ms (`-17.6%`)
- **Attached Traces**:
  - Baseline (`3fdd1f52e968`): `U:/scratch/anyps5/traces/origin-main-gpu-path/aps5-trace-3fdd1f52e968-30076.json.gz` (summary: `U:/scratch/anyps5/traces/origin-main-gpu-path/aps5-summary.jsonl`)
  - Change (`c23ffbbc59b6`): `U:/scratch/anyps5/traces/a-device-image-code-size-clean-gpu-path/aps5-trace-c23ffbbc59b6-126312.json.gz` (summary: `U:/scratch/anyps5/traces/a-device-image-code-size-clean-gpu-path/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`3fdd1f52e968` vs `c23ffbbc59b6`)**:
```json
{"commit":"3fdd1f52e968","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":337.984},"presents":20,"frame_ms":{"p50":16.898,"p90":18.484,"p99":18.740,"max":18.740},"gpu_busy_ms_per_present":5.883,"worker_busy_pct":{"queue worker 0x21":1.178,"queue worker 0x20":1.061,"queue worker 0x0":89.704},"top_phases_ms_per_frame":{"Driver.Packet":15.349,"Driver.Packet.draw":13.743,"Driver.Draw":13.554,"Driver.Draw.draw_enqueued":9.524,"Vulkan.DrawFromRecipe":8.454,"Graphics.DrawWithRecipe":8.390,"Graphics.DrawWithRecipe.draw_and_resource_release":8.367,"Recorder.Sync":4.723},"trace_dropped":false,"trace":"aps5-trace-3fdd1f52e968-30076.json.gz"}
{"commit":"c23ffbbc59b6","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":327.730},"presents":20,"frame_ms":{"p50":16.681,"p90":16.886,"p99":16.953,"max":16.953},"gpu_busy_ms_per_present":6.065,"worker_busy_pct":{"queue worker 0x21":1.144,"queue worker 0x20":1.216,"queue worker 0x0":75.801},"top_phases_ms_per_frame":{"Driver.Packet":12.649,"Driver.Packet.draw":10.800,"Driver.Draw":10.688,"Driver.Draw.draw_enqueued":7.799,"Vulkan.DrawFromRecipe":7.270,"Graphics.DrawWithRecipe":7.212,"Graphics.DrawWithRecipe.draw_and_resource_release":7.191,"Recorder.Sync":3.842},"trace_dropped":false,"trace":"aps5-trace-c23ffbbc59b6-126312.json.gz"}
```
**`python tools/trace_digest.py` — Baseline (`3fdd1f52e968`, `aps5-trace-3fdd1f52e968-30076.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.9 / p90 18.5 / p99 18.7 / max 18.7 ms, GPU busy 5.9 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 3fdd1f52e968, dropped: no

```json
{"commit":"3fdd1f52e968","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":337.984},"presents":20,"frame_ms":{"p50":16.898,"p90":18.484,"p99":18.74,"max":18.74},"gpu_busy_ms_per_present":5.883,"worker_busy_pct":{"queue worker 0x21":1.178,"queue worker 0x20":1.061,"queue worker 0x0":89.704},"top_phases_ms_per_frame":{"Driver.Packet":15.349,"Driver.Packet.draw":13.743,"Driver.Draw":13.554,"Driver.Draw.draw_enqueued":9.524,"Vulkan.DrawFromRecipe":8.454,"Graphics.DrawWithRecipe":8.39,"Graphics.DrawWithRecipe.draw_and_resource_release":8.367,"Recorder.Sync":4.723},"trace_dropped":false,"trace":"aps5-trace-3fdd1f52e968-30076.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 5: 18.7 ms at 0 ms: Driver.WorkerWait (queue worker 0x21) 16.9, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.9, Driver.WorkerWait (queue worker 0x20) 16.9, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.9
- present 24: 18.5 ms at 319 ms: Driver.Packet (queue worker 0x0) 11.8, Driver.Packet.draw (queue worker 0x0) 11.2, Driver.Draw (queue worker 0x0) 11.0, Driver.Draw.draw_enqueued (queue worker 0x0) 8.6
- present 21: 17.9 ms at 267 ms: Driver.WorkerWait (queue worker 0x21) 17.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 17.7, Driver.WorkerWait (queue worker 0x20) 17.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 17.7
- present 23: 17.6 ms at 302 ms: Driver.WorkerWait (queue worker 0x20) 17.4, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 17.4, Driver.WorkerWait (queue worker 0x21) 17.4, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 17.4
- present 7: 17.6 ms at 35 ms: Driver.WorkerWait (queue worker 0x20) 17.4, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 17.3, Driver.WorkerWait (queue worker 0x21) 17.3, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 17.3

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x21 | 0.1 (1 %) | 15.8 (94 %) | 1.0 | 0.2 | Driver.EndSubmission 0.1, Driver.Packet 0.1 |
| queue worker 0x20 | 0.1 (1 %) | 15.8 (94 %) | 1.0 | 0.1 | Driver.EndSubmission 0.1, Driver.Packet 0.1 |
| queue worker 0x0 | 9.9 (59 %) | 1.3 (8 %) | 5.6 | 20.7 | Driver.Packet 9.9 |
| draw committer | 7.9 (47 %) | 0.1 (0 %) | 9.0 | 10.4 | Vulkan.DrawFromRecipe 7.1, Graphics.Draw 0.5 |
| presenter | 0.8 (5 %) | 0.4 (2 %) | 15.7 | 0.1 | VideoOut.Flip 1.1, Driver.Present 0.1 |
| gpu | 5.9 (35 %) | 0.0 (0 %) | 11.0 | 0.0 | batch 5.9 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 32.9, Driver.WorkerWait.idle_and_completion_poll 32.9, Driver.Packet 10.1, Driver.Packet.draw 9.4, Driver.Draw 9.2, Driver.Draw.draw_enqueued 7.6, Vulkan.DrawFromRecipe 7.1, Graphics.DrawWithRecipe 7.1, Graphics.DrawWithRecipe.draw_and_resource_release 7.1, batch 5.9

**Longest waits**

- Driver.WorkerWait (queue worker 0x21) 18.1 ms at 2 ms, present 5
- Driver.WorkerWait (queue worker 0x20) 18.0 ms at 2 ms, present 5
- Driver.WorkerWait (queue worker 0x21) 17.6 ms at 268 ms, present 21
- Driver.WorkerWait (queue worker 0x20) 17.6 ms at 268 ms, present 21
- Driver.WorkerWait (queue worker 0x20) 17.4 ms at 303 ms, present 23
- Driver.WorkerWait (queue worker 0x21) 17.4 ms at 303 ms, present 23
- Driver.WorkerWait (queue worker 0x20) 17.3 ms at 36 ms, present 7
- Driver.WorkerWait (queue worker 0x20) 17.3 ms at 87 ms, present 10
- Driver.WorkerWait (queue worker 0x21) 17.2 ms at 87 ms, present 10
- Driver.WorkerWait (queue worker 0x21) 17.2 ms at 37 ms, present 7

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 1007: 1.0 ms at 282 ms, present 21: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.2, Graphics.Draw.draw_and_resource_release (draw committer) 0.1, Vulkan.DrawFromRecipe (draw committer) 0.1, Graphics.DrawWithRecipe (draw committer) 0.1
- batch 766: 0.9 ms at 214 ms, present 17: GPU no ranges; recorded by no CPU slices
- batch 405: 0.9 ms at 113 ms, present 11: GPU no ranges; recorded by Vulkan.DrawFromRecipe (draw committer) 0.1, Graphics.DrawWithRecipe (draw committer) 0.1, Graphics.DrawWithRecipe.draw_and_resource_release (draw committer) 0.1, Graphics.Draw (draw committer) 0.0
- batch 1004: 0.9 ms at 280 ms, present 21: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.0, Graphics.Draw.draw_and_resource_release (draw committer) 0.0, GpuMutex.wait.draw (draw committer) 0.0
- batch 112: 0.9 ms at 30 ms, present 6: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — With Change (`c23ffbbc59b6`, `aps5-trace-c23ffbbc59b6-126312.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.9 / p99 17.0 / max 17.0 ms, GPU busy 6.1 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit c23ffbbc59b6, dropped: no

```json
{"commit":"c23ffbbc59b6","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":327.73},"presents":20,"frame_ms":{"p50":16.681,"p90":16.886,"p99":16.953,"max":16.953},"gpu_busy_ms_per_present":6.065,"worker_busy_pct":{"queue worker 0x21":1.144,"queue worker 0x20":1.216,"queue worker 0x0":75.801},"top_phases_ms_per_frame":{"Driver.Packet":12.649,"Driver.Packet.draw":10.8,"Driver.Draw":10.688,"Driver.Draw.draw_enqueued":7.799,"Vulkan.DrawFromRecipe":7.27,"Graphics.DrawWithRecipe":7.212,"Graphics.DrawWithRecipe.draw_and_resource_release":7.191,"Recorder.Sync":3.842},"trace_dropped":false,"trace":"aps5-trace-c23ffbbc59b6-126312.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 8: 17.0 ms at 44 ms: Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8, Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7
- present 11: 16.9 ms at 94 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7
- present 19: 16.9 ms at 228 ms: Driver.WorkerWait (queue worker 0x20) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.8, Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7
- present 13: 16.9 ms at 128 ms: Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7, Driver.WorkerWait (queue worker 0x20) 16.0, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.0
- present 23: 16.7 ms at 294 ms: Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6, Driver.WorkerWait (queue worker 0x21) 16.4, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.4

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x21 | 0.1 (1 %) | 15.4 (94 %) | 0.8 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x20 | 0.1 (1 %) | 15.4 (94 %) | 0.8 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x0 | 7.7 (47 %) | 3.5 (21 %) | 5.3 | 17.2 | Driver.Packet 7.7 |
| draw committer | 6.4 (39 %) | 0.0 (0 %) | 10.0 | 9.7 | Vulkan.DrawFromRecipe 6.1, Graphics.Draw 0.2 |
| presenter | 0.7 (4 %) | 2.8 (17 %) | 13.0 | 0.0 | VideoOut.Flip 3.3, Driver.Present 0.0 |
| gpu | 6.1 (37 %) | 0.0 (0 %) | 10.3 | 0.0 | batch 6.1 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 34.2, Driver.WorkerWait.idle_and_completion_poll 34.2, Driver.Packet 7.9, Driver.Packet.draw 6.9, Driver.Draw 6.8, Driver.Draw.draw_enqueued 6.2, Vulkan.DrawFromRecipe 6.1, Graphics.DrawWithRecipe 6.1, Graphics.DrawWithRecipe.draw_and_resource_release 6.1, batch 6.1

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 229 ms, present 19
- Driver.WorkerWait (queue worker 0x21) 16.8 ms at 45 ms, present 8
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 95 ms, present 11
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 45 ms, present 8
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 95 ms, present 11
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 229 ms, present 19
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 129 ms, present 13
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 195 ms, present 17
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 79 ms, present 10
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 162 ms, present 15

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 646: 1.0 ms at 173 ms, present 15: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.1, Graphics.Draw.draw_and_resource_release (draw committer) 0.1, Resources.Prepare (draw committer) 0.1, GpuMutex.wait.draw (draw committer) 0.1
- batch 943: 1.0 ms at 257 ms, present 20: GPU no ranges; recorded by Recorder.Submit (draw committer) 0.0
- batch 704: 0.9 ms at 190 ms, present 16: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.0, Graphics.Draw.draw_and_resource_release (draw committer) 0.0
- batch 409: 0.9 ms at 104 ms, present 11: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.1, Graphics.Draw.draw_and_resource_release (draw committer) 0.1, Recorder.Submit (draw committer) 0.0
- batch 112: 0.9 ms at 24 ms, present 6: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — Interval Summary (`3fdd1f52e968` vs `c23ffbbc59b6`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
````markdown
# Baseline (3fdd1f52e968)
**3 intervals**, 0.0 to 3.0 s, 171 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 3fdd1f52e968

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 59 | 16.8 | 18.9 | 24.1 | 24.1 | 5.4 | 89 % | Driver.Packet 15.5 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 17.7 | 17.7 | 5.3 | 86 % | Driver.Packet 14.5 |
| 3 | 2-3 | 52 | 19.4 | 20.7 | 24.6 | 24.6 | 5.1 | 90 % | Driver.Packet 17.7 |

```
p99 ms  @*@  0-24.6
gpu ms  @@%  0-5.4
```
# With Change (c23ffbbc59b6)
**3 intervals**, 0.0 to 3.0 s, 181 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit c23ffbbc59b6

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.8 | 19.2 | 19.2 | 5.8 | 73 % | Driver.Packet 12.2 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 16.9 | 16.9 | 5.7 | 70 % | Driver.Packet 11.9 |
| 3 | 2-3 | 60 | 16.7 | 17.1 | 18.7 | 18.7 | 5.4 | 81 % | Driver.Packet 13.8 |

```
p99 ms  @%@  0-19.2
gpu ms  @@%  0-5.8
```
````

---

##### 2. PR onto `main` (`origin/main` `3fdd1f52e968` vs `arch/a-device-image-code-size-clean` `c23ffbbc59b6` — Default CPU Table Path)

On the default path, deduplicated `DontInline` `table_image_*` SPIR-V helpers reduce `Driver.Packet` CPU time by **10.8%** (`14.175` -> `12.651` ms/frame) and `p99` frame time from `17.643` to `16.906` ms.

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`3fdd1f52e968` -> `c23ffbbc59b6`)**:
  - `frame_ms`: p50 `16.677` -> `16.677` ms | p90 `17.288` -> `16.792` ms | p99 `17.643` -> `16.906` ms | max `17.643` -> `16.906` ms
  - `gpu_busy_ms_per_present`: `5.026` -> `6.043` ms (`20.2%`)
  - `Driver.Packet` (ms/frame): `14.175` -> `12.651` ms (`-10.8%`)
- **Attached Traces**:
  - Baseline (`3fdd1f52e968`): `U:/scratch/anyps5/traces/origin-main/aps5-trace-3fdd1f52e968-121052.json.gz` (summary: `U:/scratch/anyps5/traces/origin-main/aps5-summary.jsonl`)
  - Change (`c23ffbbc59b6`): `U:/scratch/anyps5/traces/a-device-image-code-size-clean/aps5-trace-c23ffbbc59b6-4056.json.gz` (summary: `U:/scratch/anyps5/traces/a-device-image-code-size-clean/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`3fdd1f52e968` vs `c23ffbbc59b6`)**:
```json
{"commit":"3fdd1f52e968","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":326.986},"presents":20,"frame_ms":{"p50":16.677,"p90":17.288,"p99":17.643,"max":17.643},"gpu_busy_ms_per_present":5.026,"worker_busy_pct":{"queue worker 0x20":0.935,"queue worker 0x21":0.831,"queue worker 0x0":85.718},"top_phases_ms_per_frame":{"Driver.Packet":14.175,"Driver.Packet.draw":12.722,"Driver.Draw":12.539,"Driver.Draw.draw_enqueued":8.822,"Vulkan.DrawFromRecipe":7.757,"Graphics.DrawWithRecipe":7.696,"Graphics.DrawWithRecipe.draw_and_resource_release":7.674,"Graphics.Draw":4.146},"trace_dropped":false,"trace":"aps5-trace-3fdd1f52e968-121052.json.gz"}
{"commit":"c23ffbbc59b6","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":332.888},"presents":20,"frame_ms":{"p50":16.677,"p90":16.792,"p99":16.906,"max":16.906},"gpu_busy_ms_per_present":6.043,"worker_busy_pct":{"queue worker 0x20":0.964,"queue worker 0x21":0.972,"queue worker 0x0":74.890},"top_phases_ms_per_frame":{"Driver.Packet":12.651,"Driver.Packet.draw":11.285,"Driver.Draw":11.176,"Driver.Draw.draw_enqueued":7.878,"Vulkan.DrawFromRecipe":7.205,"Graphics.DrawWithRecipe":7.148,"Graphics.DrawWithRecipe.draw_and_resource_release":7.128,"Recorder.Sync":3.951},"trace_dropped":false,"trace":"aps5-trace-c23ffbbc59b6-4056.json.gz"}
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
**`python tools/trace_digest.py` — With Change (`c23ffbbc59b6`, `aps5-trace-c23ffbbc59b6-4056.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.8 / p99 16.9 / max 16.9 ms, GPU busy 6.0 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit c23ffbbc59b6, dropped: no

```json
{"commit":"c23ffbbc59b6","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":332.888},"presents":20,"frame_ms":{"p50":16.677,"p90":16.792,"p99":16.906,"max":16.906},"gpu_busy_ms_per_present":6.043,"worker_busy_pct":{"queue worker 0x20":0.964,"queue worker 0x21":0.972,"queue worker 0x0":74.89},"top_phases_ms_per_frame":{"Driver.Packet":12.651,"Driver.Packet.draw":11.285,"Driver.Draw":11.176,"Driver.Draw.draw_enqueued":7.878,"Vulkan.DrawFromRecipe":7.205,"Graphics.DrawWithRecipe":7.148,"Graphics.DrawWithRecipe.draw_and_resource_release":7.128,"Recorder.Sync":3.951},"trace_dropped":false,"trace":"aps5-trace-c23ffbbc59b6-4056.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 12: 16.9 ms at 116 ms: Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8, Driver.WorkerWait (queue worker 0x20) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.8
- present 7: 16.8 ms at 33 ms: Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6, Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6
- present 22: 16.7 ms at 283 ms: Driver.WorkerWait (queue worker 0x20) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.5, Driver.WorkerWait (queue worker 0x21) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.5
- present 11: 16.7 ms at 99 ms: Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6, Driver.WorkerWait (queue worker 0x20) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.5
- present 19: 16.7 ms at 233 ms: Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x21 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x0 | 7.1 (43 %) | 3.6 (22 %) | 5.9 | 20.5 | Driver.Packet 7.1 |
| draw committer | 6.4 (38 %) | 0.0 (0 %) | 10.2 | 9.6 | Vulkan.DrawFromRecipe 6.1, Graphics.Draw 0.2 |
| presenter | 0.8 (5 %) | 2.9 (17 %) | 13.0 | 0.1 | VideoOut.Flip 3.4, Driver.Present 0.1 |
| gpu | 5.7 (34 %) | 0.0 (0 %) | 10.9 | 0.0 | batch 5.7 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 34.9, Driver.WorkerWait.idle_and_completion_poll 34.9, Driver.Packet 7.3, Driver.Packet.draw 6.8, Driver.Draw 6.8, Driver.Draw.draw_enqueued 6.2, Vulkan.DrawFromRecipe 6.1, Graphics.DrawWithRecipe 6.0, Graphics.DrawWithRecipe.draw_and_resource_release 6.0, batch 5.7

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 17.2 ms at 234 ms, present 19
- Driver.WorkerWait (queue worker 0x21) 17.1 ms at 234 ms, present 19
- Driver.WorkerWait (queue worker 0x21) 16.9 ms at 267 ms, present 21
- Driver.WorkerWait (queue worker 0x20) 16.9 ms at 267 ms, present 21
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 117 ms, present 12
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 34 ms, present 7
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 201 ms, present 17
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 34 ms, present 7
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 117 ms, present 12
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 67 ms, present 9

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 53: 1.0 ms at 11 ms, present 5: GPU no ranges; recorded by no CPU slices
- batch 350: 0.9 ms at 94 ms, present 10: GPU no ranges; recorded by no CPU slices
- batch 292: 0.9 ms at 77 ms, present 9: GPU no ranges; recorded by no CPU slices
- batch 821: 0.9 ms at 227 ms, present 18: GPU no ranges; recorded by no CPU slices
- batch 1119: 0.9 ms at 310 ms, present 23: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — Interval Summary (`3fdd1f52e968` vs `c23ffbbc59b6`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
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
# With Change (c23ffbbc59b6)
**3 intervals**, 0.0 to 3.0 s, 179 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit c23ffbbc59b6

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.9 | 18.3 | 18.3 | 5.8 | 73 % | Driver.Packet 12.3 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 17.0 | 17.0 | 5.6 | 72 % | Driver.Packet 12.1 |
| 3 | 2-3 | 58 | 17.5 | 19.2 | 20.6 | 20.6 | 5.3 | 87 % | Driver.Packet 15.4 |

```
p99 ms  %#@  0-20.6
gpu ms  @@%  0-5.8
```
````

---

##### 3. Stacked on `#7642` (`pr/7642` `05502ba6a504` vs `43b7a6d8da5c` — `Depends on: #6946, #6947, #7642`, Default & `APS5_GPU_PATH=1`)

Stacked directly on `#7642` (`05502ba6a504`). Also captured with `APS5_GPU_PATH=1` (`U:/scratch/anyps5/traces/pr-7642-gpu-path` vs `U:/scratch/anyps5/traces/a-device-image-code-size-on-7642-gpu-path`):

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`05502ba6a504` -> `43b7a6d8da5c`)**:
  - `frame_ms`: p50 `16.739` -> `16.689` ms | p90 `16.873` -> `16.849` ms | p99 `16.926` -> `16.896` ms | max `16.926` -> `16.896` ms
  - `gpu_busy_ms_per_present`: `5.585` -> `5.866` ms (`5.0%`)
  - `Driver.Packet` (ms/frame): `8.845` -> `8.943` ms (`1.1%`)
- **Attached Traces**:
  - Baseline (`05502ba6a504`): `U:/scratch/anyps5/traces/pr-7642/aps5-trace-05502ba6a504-57400.json.gz` (summary: `U:/scratch/anyps5/traces/pr-7642/aps5-summary.jsonl`)
  - Change (`43b7a6d8da5c`): `U:/scratch/anyps5/traces/a-device-image-code-size-on-7642/aps5-trace-43b7a6d8da5c-45300.json.gz` (summary: `U:/scratch/anyps5/traces/a-device-image-code-size-on-7642/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`05502ba6a504` vs `43b7a6d8da5c`)**:
```json
{"commit":"05502ba6a504","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.522},"presents":20,"frame_ms":{"p50":16.739,"p90":16.873,"p99":16.926,"max":16.926},"gpu_busy_ms_per_present":5.585,"worker_busy_pct":{"queue worker 0x20":1.074,"queue worker 0x21":1.168,"queue worker 0x0":51.797},"top_phases_ms_per_frame":{"Driver.Packet":8.845,"Driver.Packet.draw":7.425,"Driver.Draw":7.308,"VideoOut.Flip":6.966,"VideoOut.Flip.vblank_wait":6.171,"Driver.Draw.draw_enqueued":3.407,"Driver.Draw.shader_compile_and_link":1.308,"Driver.Draw.draw_and_resource_release":1.295},"trace_dropped":false,"trace":"aps5-trace-05502ba6a504-57400.json.gz"}
{"commit":"43b7a6d8da5c","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.456},"presents":20,"frame_ms":{"p50":16.689,"p90":16.849,"p99":16.896,"max":16.896},"gpu_busy_ms_per_present":5.866,"worker_busy_pct":{"queue worker 0x20":1.257,"queue worker 0x21":1.283,"queue worker 0x0":52.130},"top_phases_ms_per_frame":{"Driver.Packet":8.943,"Driver.Packet.draw":7.484,"Driver.Draw":7.344,"VideoOut.Flip":6.919,"VideoOut.Flip.vblank_wait":6.001,"Driver.Draw.draw_enqueued":3.557,"Driver.Draw.draw_and_resource_release":1.316,"Graphics.Draw":1.296},"trace_dropped":false,"trace":"aps5-trace-43b7a6d8da5c-45300.json.gz"}
```
**`python tools/trace_digest.py` — Baseline (`05502ba6a504`, `aps5-trace-05502ba6a504-57400.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.9 / p99 16.9 / max 16.9 ms, GPU busy 5.6 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 05502ba6a504, dropped: no

```json
{"commit":"05502ba6a504","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.522},"presents":20,"frame_ms":{"p50":16.739,"p90":16.873,"p99":16.926,"max":16.926},"gpu_busy_ms_per_present":5.585,"worker_busy_pct":{"queue worker 0x20":1.074,"queue worker 0x21":1.168,"queue worker 0x0":51.797},"top_phases_ms_per_frame":{"Driver.Packet":8.845,"Driver.Packet.draw":7.425,"Driver.Draw":7.308,"VideoOut.Flip":6.966,"VideoOut.Flip.vblank_wait":6.171,"Driver.Draw.draw_enqueued":3.407,"Driver.Draw.shader_compile_and_link":1.308,"Driver.Draw.draw_and_resource_release":1.295},"trace_dropped":false,"trace":"aps5-trace-05502ba6a504-57400.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 7: 16.9 ms at 33 ms: Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7, Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6
- present 10: 16.9 ms at 83 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7
- present 21: 16.8 ms at 267 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6
- present 22: 16.8 ms at 284 ms: Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7, Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6
- present 17: 16.8 ms at 200 ms: Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6, Driver.WorkerWait (queue worker 0x21) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.5

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x21 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.2 | Driver.EndSubmission 0.1, Driver.Packet 0.1 |
| queue worker 0x0 | 3.3 (20 %) | 7.3 (44 %) | 6.1 | 21.1 | Driver.Packet 3.3, Driver.EndSubmission 0.0 |
| presenter | 0.7 (4 %) | 6.7 (40 %) | 9.3 | 0.1 | VideoOut.Flip 7.0, Driver.Present 0.1 |
| gpu | 5.6 (34 %) | 0.0 (0 %) | 11.1 | 0.0 | batch 5.6 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 38.6, Driver.WorkerWait.idle_and_completion_poll 38.6, VideoOut.Flip 7.0, VideoOut.Flip.vblank_wait 6.2, batch 5.6, Driver.Packet 3.5, Driver.Packet.draw 2.8, Driver.Draw 2.8, Driver.Draw.draw_enqueued 1.3, Driver.Present 1.1

**Longest waits**

- Driver.WorkerWait (queue worker 0x21) 16.9 ms at 285 ms, present 22
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 285 ms, present 22
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 68 ms, present 9
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 35 ms, present 7
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 201 ms, present 17
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 268 ms, present 21
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 68 ms, present 9
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 218 ms, present 18
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 35 ms, present 7
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 268 ms, present 21

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 95: 1.0 ms at 108 ms, present 11: GPU no ranges; recorded by no CPU slices
- batch 150: 1.0 ms at 174 ms, present 15: GPU no ranges; recorded by no CPU slices
- batch 109: 0.9 ms at 124 ms, present 12: GPU no ranges; recorded by no CPU slices
- batch 248: 0.9 ms at 291 ms, present 22: GPU no ranges; recorded by no CPU slices
- batch 220: 0.9 ms at 258 ms, present 20: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — With Change (`43b7a6d8da5c`, `aps5-trace-43b7a6d8da5c-45300.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.8 / p99 16.9 / max 16.9 ms, GPU busy 5.9 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 43b7a6d8da5c, dropped: no

```json
{"commit":"43b7a6d8da5c","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.456},"presents":20,"frame_ms":{"p50":16.689,"p90":16.849,"p99":16.896,"max":16.896},"gpu_busy_ms_per_present":5.866,"worker_busy_pct":{"queue worker 0x20":1.257,"queue worker 0x21":1.283,"queue worker 0x0":52.13},"top_phases_ms_per_frame":{"Driver.Packet":8.943,"Driver.Packet.draw":7.484,"Driver.Draw":7.344,"VideoOut.Flip":6.919,"VideoOut.Flip.vblank_wait":6.001,"Driver.Draw.draw_enqueued":3.557,"Driver.Draw.draw_and_resource_release":1.316,"Graphics.Draw":1.296},"trace_dropped":false,"trace":"aps5-trace-43b7a6d8da5c-45300.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 9: 16.9 ms at 67 ms: Driver.WorkerWait (queue worker 0x21) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.5, Driver.WorkerWait (queue worker 0x20) 16.4, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.4
- present 7: 16.8 ms at 33 ms: Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6, Driver.WorkerWait (queue worker 0x21) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.5
- present 20: 16.8 ms at 250 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7
- present 18: 16.8 ms at 217 ms: Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6, Driver.WorkerWait (queue worker 0x20) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.4
- present 14: 16.8 ms at 150 ms: Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6, Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.7 (94 %) | 0.8 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x21 | 0.1 (1 %) | 15.7 (94 %) | 0.8 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x0 | 3.4 (20 %) | 7.2 (43 %) | 6.0 | 21.1 | Driver.Packet 3.4, Driver.EndSubmission 0.0 |
| presenter | 0.7 (4 %) | 6.5 (39 %) | 9.4 | 0.1 | VideoOut.Flip 6.9, Driver.Present 0.1 |
| gpu | 5.9 (35 %) | 0.0 (0 %) | 10.8 | 0.0 | batch 5.9 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 38.5, Driver.WorkerWait.idle_and_completion_poll 38.5, VideoOut.Flip 6.9, VideoOut.Flip.vblank_wait 6.0, batch 5.9, Driver.Packet 3.6, Driver.Packet.draw 2.9, Driver.Draw 2.8, Driver.Draw.draw_enqueued 1.3, Driver.Present 1.1

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 16.9 ms at 102 ms, present 11
- Driver.WorkerWait (queue worker 0x21) 16.8 ms at 102 ms, present 11
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 35 ms, present 7
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 68 ms, present 9
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 251 ms, present 20
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 302 ms, present 23
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 285 ms, present 22
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 252 ms, present 20
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 285 ms, present 22
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 302 ms, present 23

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 95: 1.1 ms at 108 ms, present 11: GPU no ranges; recorded by no CPU slices
- batch 122: 1.0 ms at 141 ms, present 13: GPU no ranges; recorded by no CPU slices
- batch 10: 1.0 ms at 8 ms, present 5: GPU no ranges; recorded by no CPU slices
- batch 39: 0.9 ms at 41 ms, present 7: GPU no ranges; recorded by no CPU slices
- batch 206: 0.9 ms at 241 ms, present 19: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — Interval Summary (`05502ba6a504` vs `43b7a6d8da5c`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
````markdown
# Baseline (05502ba6a504)
**3 intervals**, 0.0 to 3.0 s, 181 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 05502ba6a504

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.9 | 17.2 | 17.2 | 5.8 | 52 % | Driver.Packet 8.8 |
| 2 | 1-2 | 60 | 16.7 | 16.8 | 17.0 | 17.0 | 5.7 | 49 % | Driver.Packet 8.4 |
| 3 | 2-3 | 60 | 16.7 | 16.9 | 16.9 | 16.9 | 5.7 | 63 % | Driver.Packet 10.7 |

```
p99 ms  @@@  0-17.2
gpu ms  @@@  0-5.8
```
# With Change (43b7a6d8da5c)
**3 intervals**, 0.0 to 3.0 s, 181 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 43b7a6d8da5c

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.8 | 17.1 | 17.1 | 6.0 | 53 % | Driver.Packet 8.9 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 17.0 | 17.0 | 6.0 | 51 % | Driver.Packet 8.8 |
| 3 | 2-3 | 60 | 16.7 | 16.8 | 17.1 | 17.1 | 5.9 | 66 % | Driver.Packet 11.2 |

```
p99 ms  @@@  0-17.1
gpu ms  @@@  0-6.0
```
````

---

##### 4. Stacked on `#7642` (`pr/7642` `05502ba6a504` vs `43b7a6d8da5c` — `APS5_GPU_PATH=1`)
- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`05502ba6a504` -> `43b7a6d8da5c`)**:
  - `frame_ms`: p50 `16.683` -> `16.689` ms | p90 `16.926` -> `16.873` ms | p99 `16.933` -> `16.993` ms | max `16.933` -> `16.993` ms
  - `gpu_busy_ms_per_present`: `5.743` -> `5.826` ms (`1.4%`)
  - `Driver.Packet` (ms/frame): `8.277` -> `9.169` ms (`10.8%`)
- **Attached Traces**:
  - Baseline (`05502ba6a504`): `U:/scratch/anyps5/traces/pr-7642-gpu-path/aps5-trace-05502ba6a504-30580.json.gz` (summary: `U:/scratch/anyps5/traces/pr-7642-gpu-path/aps5-summary.jsonl`)
  - Change (`43b7a6d8da5c`): `U:/scratch/anyps5/traces/a-device-image-code-size-on-7642-gpu-path/aps5-trace-43b7a6d8da5c-127748.json.gz` (summary: `U:/scratch/anyps5/traces/a-device-image-code-size-on-7642-gpu-path/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`05502ba6a504` vs `43b7a6d8da5c`)**:
```json
{"commit":"05502ba6a504","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.356},"presents":20,"frame_ms":{"p50":16.683,"p90":16.926,"p99":16.933,"max":16.933},"gpu_busy_ms_per_present":5.743,"worker_busy_pct":{"queue worker 0x20":0.917,"queue worker 0x21":0.931,"queue worker 0x0":48.710},"top_phases_ms_per_frame":{"Driver.Packet":8.277,"VideoOut.Flip":7.511,"Driver.Packet.draw":6.899,"VideoOut.Flip.vblank_wait":6.796,"Driver.Draw":6.783,"Driver.Draw.draw_enqueued":3.658,"Driver.Draw.shader_compile_and_link":1.309,"Driver.Present":1.053},"trace_dropped":false,"trace":"aps5-trace-05502ba6a504-30580.json.gz"}
{"commit":"43b7a6d8da5c","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.529},"presents":20,"frame_ms":{"p50":16.689,"p90":16.873,"p99":16.993,"max":16.993},"gpu_busy_ms_per_present":5.826,"worker_busy_pct":{"queue worker 0x20":0.942,"queue worker 0x21":1.058,"queue worker 0x0":53.871},"top_phases_ms_per_frame":{"Driver.Packet":9.169,"Driver.Packet.draw":7.620,"Driver.Draw":7.490,"VideoOut.Flip":6.581,"VideoOut.Flip.vblank_wait":5.618,"Driver.Draw.draw_enqueued":3.706,"Driver.Draw.shader_compile_and_link":1.566,"Driver.Present":1.120},"trace_dropped":false,"trace":"aps5-trace-43b7a6d8da5c-127748.json.gz"}
```
**`python tools/trace_digest.py` — Baseline (`05502ba6a504`, `aps5-trace-05502ba6a504-30580.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.9 / p99 16.9 / max 16.9 ms, GPU busy 5.7 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 05502ba6a504, dropped: no

```json
{"commit":"05502ba6a504","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.356},"presents":20,"frame_ms":{"p50":16.683,"p90":16.926,"p99":16.933,"max":16.933},"gpu_busy_ms_per_present":5.743,"worker_busy_pct":{"queue worker 0x20":0.917,"queue worker 0x21":0.931,"queue worker 0x0":48.71},"top_phases_ms_per_frame":{"Driver.Packet":8.277,"VideoOut.Flip":7.511,"Driver.Packet.draw":6.899,"VideoOut.Flip.vblank_wait":6.796,"Driver.Draw":6.783,"Driver.Draw.draw_enqueued":3.658,"Driver.Draw.shader_compile_and_link":1.309,"Driver.Present":1.053},"trace_dropped":false,"trace":"aps5-trace-05502ba6a504-30580.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 12: 16.9 ms at 116 ms: Driver.WorkerWait (queue worker 0x20) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.8, Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8
- present 22: 16.9 ms at 283 ms: Driver.WorkerWait (queue worker 0x20) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.8, Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8
- present 14: 16.9 ms at 150 ms: Driver.WorkerWait (queue worker 0x20) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7
- present 9: 16.8 ms at 66 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 16: 16.8 ms at 183 ms: Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6, Driver.WorkerWait (queue worker 0x20) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.5

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x21 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.1 | Driver.EndSubmission 0.1, Driver.Packet 0.1 |
| queue worker 0x0 | 2.7 (16 %) | 7.8 (47 %) | 6.2 | 20.0 | Driver.Packet 2.7, Driver.EndSubmission 0.0 |
| presenter | 0.6 (4 %) | 7.3 (44 %) | 8.7 | 0.1 | VideoOut.Flip 7.5, Driver.Present 0.1 |
| gpu | 5.7 (34 %) | 0.0 (0 %) | 10.9 | 0.0 | batch 5.7 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 39.2, Driver.WorkerWait.idle_and_completion_poll 39.1, VideoOut.Flip 7.5, VideoOut.Flip.vblank_wait 6.8, batch 5.7, Driver.Packet 2.8, Driver.Packet.draw 2.3, Driver.Draw 2.2, Driver.Draw.draw_enqueued 1.5, Driver.Present 1.1

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 284 ms, present 22
- Driver.WorkerWait (queue worker 0x21) 16.8 ms at 284 ms, present 22
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 151 ms, present 14
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 51 ms, present 8
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 118 ms, present 12
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 51 ms, present 8
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 151 ms, present 14
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 118 ms, present 12
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 184 ms, present 16
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 234 ms, present 19

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 165: 0.9 ms at 191 ms, present 16: GPU no ranges; recorded by no CPU slices
- batch 161: 0.9 ms at 188 ms, present 16: GPU no ranges; recorded by no CPU slices
- batch 178: 0.9 ms at 207 ms, present 17: GPU no ranges; recorded by no CPU slices
- batch 263: 0.9 ms at 309 ms, present 23: GPU no ranges; recorded by no CPU slices
- batch 81: 0.9 ms at 91 ms, present 10: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — With Change (`43b7a6d8da5c`, `aps5-trace-43b7a6d8da5c-127748.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.9 / p99 17.0 / max 17.0 ms, GPU busy 5.8 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 43b7a6d8da5c, dropped: no

```json
{"commit":"43b7a6d8da5c","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.529},"presents":20,"frame_ms":{"p50":16.689,"p90":16.873,"p99":16.993,"max":16.993},"gpu_busy_ms_per_present":5.826,"worker_busy_pct":{"queue worker 0x20":0.942,"queue worker 0x21":1.058,"queue worker 0x0":53.871},"top_phases_ms_per_frame":{"Driver.Packet":9.169,"Driver.Packet.draw":7.62,"Driver.Draw":7.49,"VideoOut.Flip":6.581,"VideoOut.Flip.vblank_wait":5.618,"Driver.Draw.draw_enqueued":3.706,"Driver.Draw.shader_compile_and_link":1.566,"Driver.Present":1.12},"trace_dropped":false,"trace":"aps5-trace-43b7a6d8da5c-127748.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 21: 17.0 ms at 267 ms: Driver.WorkerWait (queue worker 0x20) 16.9, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.9, Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8
- present 13: 16.9 ms at 133 ms: Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7, Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6
- present 8: 16.9 ms at 50 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7
- present 16: 16.8 ms at 183 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7
- present 5: 16.8 ms at 0 ms: Driver.WorkerWait (queue worker 0x20) 14.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 14.8, Driver.WorkerWait (queue worker 0x21) 14.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 14.8

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x21 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.2 | Driver.EndSubmission 0.1, Driver.Packet 0.1 |
| queue worker 0x0 | 3.3 (20 %) | 6.9 (41 %) | 6.4 | 21.0 | Driver.Packet 3.3, Recorder.Reap 0.0 |
| presenter | 0.8 (5 %) | 6.1 (37 %) | 9.7 | 0.1 | VideoOut.Flip 6.6, Driver.Present 0.1 |
| gpu | 5.8 (35 %) | 0.0 (0 %) | 10.9 | 0.0 | batch 5.8 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 38.2, Driver.WorkerWait.idle_and_completion_poll 38.2, VideoOut.Flip 6.6, batch 5.8, VideoOut.Flip.vblank_wait 5.6, Driver.Packet 3.5, Driver.Packet.draw 2.8, Driver.Draw 2.8, Driver.Draw.draw_enqueued 1.7, Driver.Present 1.1

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 17.5 ms at 118 ms, present 12
- Driver.WorkerWait (queue worker 0x21) 17.3 ms at 118 ms, present 12
- Driver.WorkerWait (queue worker 0x20) 17.0 ms at 268 ms, present 21
- Driver.WorkerWait (queue worker 0x21) 16.8 ms at 268 ms, present 21
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 35 ms, present 7
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 35 ms, present 7
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 251 ms, present 20
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 218 ms, present 18
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 218 ms, present 18
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 85 ms, present 10

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 123: 0.9 ms at 143 ms, present 13: GPU no ranges; recorded by no CPU slices
- batch 4: 0.9 ms at 4 ms, present 5: GPU no ranges; recorded by no CPU slices
- batch 63: 0.8 ms at 73 ms, present 9: GPU no ranges; recorded by no CPU slices
- batch 260: 0.8 ms at 307 ms, present 23: GPU no ranges; recorded by no CPU slices
- batch 177: 0.8 ms at 207 ms, present 17: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — Interval Summary (`05502ba6a504` vs `43b7a6d8da5c`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
````markdown
# Baseline (05502ba6a504)
**3 intervals**, 0.0 to 3.0 s, 181 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 05502ba6a504

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.9 | 17.1 | 17.1 | 5.9 | 48 % | Driver.Packet 8.2 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 16.9 | 16.9 | 5.8 | 47 % | VideoOut.Flip 8.4 |
| 3 | 2-3 | 60 | 16.7 | 16.9 | 17.3 | 17.3 | 5.8 | 59 % | Driver.Packet 10.0 |

```
p99 ms  @@@  0-17.3
gpu ms  @@@  0-5.9
```
# With Change (43b7a6d8da5c)
**3 intervals**, 0.0 to 3.0 s, 181 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 43b7a6d8da5c

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.9 | 17.0 | 17.0 | 6.4 | 53 % | Driver.Packet 9.0 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 17.0 | 17.0 | 6.3 | 48 % | Driver.Packet 8.3 |
| 3 | 2-3 | 60 | 16.7 | 16.9 | 17.0 | 17.0 | 6.1 | 61 % | Driver.Packet 10.3 |

```
p99 ms  @@@  0-17.0
gpu ms  @@%  0-6.4
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
- [x] Depends on: #6946, #6947, #7642
- [x] Fixes: none
- [x] Unblocks: PPSA01556
- [x] GPUs tested: NVIDIA GeForce RTX 5090 (driver 610.52, Windows 11)
- [x] AI-assisted: yes
