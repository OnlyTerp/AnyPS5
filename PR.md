### What

Compute and graphics pipelines are created outside `GuestMemory::GpuMutex` across multiple queue workers, background draw-pipeline workers, and journal replay workers. Because a host driver's SPIR-V compiler heap scales with module size (~12 KiB per SPIR-V word, or ~12 GiB for a 2.7M-word module), unbounded concurrent pipeline creations can compound peak compiler allocations and exhaust host memory or push the process past the 16 GiB RAM-side ceiling.

This change introduces `AgcDriver::CreationGate` and `AgcDriver::CreationAhead` to bound concurrent pipeline compilations by both estimated compiler memory and live RAM-side memory headroom:

- **Compiler Memory Budget (`CreationGate`)**: Estimates each creation's compiler footprint at `12,288` bytes per SPIR-V word (`CompilerBytesPerWord`) against a default budget of `1/16` of physical memory (overridable via `APS5_PIPELINE_CREATION_BUDGET_MIB`). A creation larger than the budget runs alone when `inFlight == 0`.
- **Frame Priority vs Ahead-of-Frame Work (`CreationAhead`)**: Prebuild and journal-replay workers mark their scope with `CreationAhead`, which queues behind a FIFO ticket lock and yields whenever any in-frame creation is waiting (`framesWaiting > 0`). In-frame creations wait only when their words do not fit alongside in-flight creations and take priority ahead of queued background creations as soon as running compilations finish.
- **RAM-Side Headroom & Device-Local Backing Subtraction (`RamSideBytes`, `UseRamSide`)**: Measures process private commit plus guest arena mapped views (`MEM_MAPPED`) minus `VK_EXT_memory_budget` device-local heap usage (`CreationMemory::Install` in `VulkanDevice`). Concurrent creations are held when `RamSideBytes() + EstimateBytes(inFlight + words)` would exceed `16 GiB` (`MemoryLimitBytes`), while a single creation (`inFlight == 0`) is always permitted to make forward progress (`OverMemory()`).
- **Compute Replay Sequence Mode**: Extends `agc_compute_pipeline_replay` with `--sequence <threads> <hold> [--budget-creations <n>]` to benchmark multi-threaded compute pipeline compilation concurrency and peak private memory under default and constrained budgets.

### Tested

- **Red (gate & budget checks bypassed)**:
  - `agc_pipeline_creation_gate`: fails with `creations together over the words in flight ran at the same time` (`0.06s`).
  - `agc_compute_pipeline_budget`: fails with `FAIL: four threads with a budget of one creation: 4 creations ran at once, expected one at a time` and `FAIL: four threads with a budget of one creation: the peak private memory grew 169.700000 MiB over the device, the one-thread run's 61.900000 MiB, allowed 141.375000 MiB` (`4.55s`).
- **Green (with `CreationGate` and `CreationMemory::Install` active)**:
  - `ctest --test-dir build -R "^(agc_pipeline_creation_gate|agc_driver_creation_gate_device_local|agc_compute_pipeline_budget)$" --output-on-failure`:
    - `agc_driver_creation_gate_device_local`: Passed (`0.46s`, `private +513 MiB, RAM side +1 MiB`).
    - `agc_pipeline_creation_gate`: Passed (`1.36s`).
    - `agc_compute_pipeline_budget`: Passed (`6.32s`, `one thread peak 1 creations, 62.4 MiB over the device; four threads under one creation's budget peak 1, 87.9 MiB; four threads under the default budget peak 4, 172.3 MiB`).
  - `python tools/check_conventions.py --base origin/main --head HEAD`: `0 findings`.
  - `python tools/check_test_registration.py build`: `0 errors`.


#### Performance report

##### 1. PR onto `main` (`origin/main` `3fdd1f52e968` vs `arch/b-pipeline-memory-gate-clean` `a01477c5e2dd`)

`AgcDriver::CreationGate` and `CreationAhead` bound concurrent SPIR-V pipeline compilations by compiler memory footprint and RAM-side headroom while prioritizing in-frame creations ahead of background prebuild workers (`Driver.Packet` `14.175` -> `12.942` ms/frame, **-8.7%**, `p99` `17.643` -> `16.959` ms).

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`3fdd1f52e968` -> `a01477c5e2dd`)**:
  - `frame_ms`: p50 `16.677` -> `16.681` ms | p90 `17.288` -> `16.959` ms | p99 `17.643` -> `16.959` ms | max `17.643` -> `16.959` ms
  - `gpu_busy_ms_per_present`: `5.026` -> `5.78` ms (`15.0%`)
  - `Driver.Packet` (ms/frame): `14.175` -> `12.942` ms (`-8.7%`)
- **Attached Traces**:
  - Baseline (`3fdd1f52e968`): `U:/scratch/anyps5/traces/origin-main/aps5-trace-3fdd1f52e968-121052.json.gz` (summary: `U:/scratch/anyps5/traces/origin-main/aps5-summary.jsonl`)
  - Change (`a01477c5e2dd`): `U:/scratch/anyps5/traces/b-pipeline-memory-gate-clean/aps5-trace-a01477c5e2dd-123612.json.gz` (summary: `U:/scratch/anyps5/traces/b-pipeline-memory-gate-clean/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`3fdd1f52e968` vs `a01477c5e2dd`)**:
```json
{"commit":"3fdd1f52e968","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":326.986},"presents":20,"frame_ms":{"p50":16.677,"p90":17.288,"p99":17.643,"max":17.643},"gpu_busy_ms_per_present":5.026,"worker_busy_pct":{"queue worker 0x20":0.935,"queue worker 0x21":0.831,"queue worker 0x0":85.718},"top_phases_ms_per_frame":{"Driver.Packet":14.175,"Driver.Packet.draw":12.722,"Driver.Draw":12.539,"Driver.Draw.draw_enqueued":8.822,"Vulkan.DrawFromRecipe":7.757,"Graphics.DrawWithRecipe":7.696,"Graphics.DrawWithRecipe.draw_and_resource_release":7.674,"Graphics.Draw":4.146},"trace_dropped":false,"trace":"aps5-trace-3fdd1f52e968-121052.json.gz"}
{"commit":"a01477c5e2dd","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":330.562},"presents":20,"frame_ms":{"p50":16.681,"p90":16.959,"p99":16.959,"max":16.959},"gpu_busy_ms_per_present":5.780,"worker_busy_pct":{"queue worker 0x20":0.752,"queue worker 0x21":0.920,"queue worker 0x0":77.308},"top_phases_ms_per_frame":{"Driver.Packet":12.942,"Driver.Packet.draw":11.507,"Driver.Draw":11.317,"Driver.Draw.draw_enqueued":8.019,"Vulkan.DrawFromRecipe":7.022,"Graphics.DrawWithRecipe":6.965,"Graphics.DrawWithRecipe.draw_and_resource_release":6.944,"Graphics.Draw":3.936},"trace_dropped":false,"trace":"aps5-trace-a01477c5e2dd-123612.json.gz"}
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
**`python tools/trace_digest.py` — With Change (`a01477c5e2dd`, `aps5-trace-a01477c5e2dd-123612.json.gz`)**:
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
**`python tools/trace_digest.py` — Interval Summary (`3fdd1f52e968` vs `a01477c5e2dd`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
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
# With Change (a01477c5e2dd)
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

