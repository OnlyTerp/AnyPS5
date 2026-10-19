Depends on: #7643

### What

#### Cause

Three paths in `libSceAgcDriver` still perform expensive graphics pipeline creation or redundant DCC metadata key decoding while holding `GuestMemory::GpuMutex()` or serialize independent pipeline library builds across threads:

1. **Graphics pipeline library and monolithic pipeline creation under mutexes and the GPU lock**:
   - `PipelineLibraryCache::getPart` held `PipelineLibraryCache::mutex` across `createLibrary(...)` (`vkCreateGraphicsPipelines`), and `Impl::getOrCreatePipeline` in `Pipeline.cpp` held `Impl::mutex` across `buildGraphicsPipeline(...)` (including stage module creation, pipeline library part acquisition, fast-link, and monolithic fallback `vkCreateGraphicsPipelines`), preventing concurrent threads from building independent pipeline libraries or looking up already-cached pipelines while a new pipeline build was in flight.
   - `Driver::draw` did not pre-build graphics pipelines before acquiring `GuestMemory::GpuMutex()` (or before enqueuing `commitDraw` on the pipelined draw queue introduced in #7643), causing first-seen vertex/fragment program pairs and pipeline state combinations to compile and link Vulkan graphics pipeline libraries while holding the global GPU lock.
2. **Redundant DCC metadata key re-decoding during unlocked draw/compute pre-sync**:
   - `PresyncGuestBufferResources` re-decoded 64-bit `DccKey` digests from guest DCC metadata pages even when `DccMetadata` already held a complete cached `DccKey` for the `(colorAddress, dccAddress, dccSize)` tuple with no intervening guest write or GPU DCC write, and re-took `GuestMemory::GpuMutex()` inside `ShaderResources` on the locked path when no DCC write was pending.

#### Changes

- **Per-key in-flight synchronization in `PipelineLibrary.cpp` and `Pipeline.cpp`**:
  - Replace single-mutex serialization across `createLibrary` in `PipelineLibraryCache::getPart` with per-`PartKey` `InFlightPart` condition-variable waiting so independent pipeline library parts build concurrently outside `PipelineLibraryCache::mutex` while duplicate requests for the same key wait for the single in-flight build. Wait for in-flight builds before destroying libraries in `ClearPipelineLibraries`.
  - Add `builtUnderLock` counters to `PipelineLibraryCounters` (tracked via `GuestMemory::GpuMutexHeldByCurrentThread()`) alongside `built` and `linked`.
  - Replace mutex-held `buildGraphicsPipeline` in `Pipeline.cpp` with per-`PipelineKey` `InFlightPipeline` condition-variable waiting and a dedicated `shaderModuleMutex` around the stage module cache so cached pipeline lookups and independent pipeline builds proceed without blocking on an in-flight `vkCreateGraphicsPipelines`.
- **Unlocked draw pipeline preparation (`PrepareDrawPipelines`)**:
  - Add `PreparePipelines(const Context&, const State&, std::span<const CompiledShader>)` in `Pipeline.cpp` / `PipelineLibrary.hpp` and expose `VulkanDevice::PrepareDrawPipelines(const Graphics::State&, std::span<const Graphics::CompiledShader>)`.
  - Invoke `localDevice->PrepareDrawPipelines(graphics, stages)` before acquiring `GuestMemory::GpuMutex()` in `Driver::draw` (both on the pipelined draw path before enqueuing `commitDraw` and in `lockForDraw()` when `!gpuLock.owns_lock() && !drawHit`).
- **Cached DCC key reuse across unlocked pre-sync (`DccMetadata` / `GuestBufferMemory` / `ShaderResources`)**:
  - Add `HasCompleteDccKey(VkDevice, uint64_t colorAddress, uint64_t dccAddress, uint64_t dccSize)` to `DccMetadata` and invalidate matching `dccKeys` entries in `NoteSampledDccWrite`.
  - Update `PresyncGuestBufferResources` to skip re-decoding `DccKey` when `HasCompleteDccKey` is already satisfied and no guest write is pending on the DCC range, while still recording newly decoded keys via `RecordSampledDccKey`.
  - Split `ShaderResources::syncSampledDccTextures` into `presyncSampledDccTextures()` (called from the constructor when `!GuestMemory::GpuMutexHeldByCurrentThread()`) and `syncSampledDccTextures(bool lockGpu)` (called from `Complete()`, avoiding redundant `GpuMutex` acquisition when already under the GPU lock).
- **Tests**:
  - Add `pipelineLibraryConcurrencyTests` and `cachedPipelineConcurrencyTests` to `core/libs/prx/libSceAgcDriver/tests/Graphics.cpp`.
  - Add `core/libs/prx/libSceAgcDriver/tests/execution/DrawPipelinePreparation.cpp` (`agc_driver_draw_pipeline_preparation`) and `core/libs/prx/libSceAgcDriver/tests/execution/DrawPipelineOffLock.cpp` (`agc_driver_draw_pipeline_off_lock`).

### Tested

- **Red -> Green Verification**:
  - `agc_driver_draw_pipeline_off_lock`:
    - **Red** (without `PrepareDrawPipelines` on the pipelined draw path in `Driver::draw`): fails with `AGC graphics: the first draw of a new program pair built 0/1/1/0 library parts while holding the GPU lock`.
    - **Green** (with `PrepareDrawPipelines` before `commitDraw` / `gpuLock.lock()`): passes in `0.63s` (`draw pipeline off-lock tests passed`), building `0/1/1/0` new library parts off-lock and `0/0/0/0` while holding `GuestMemory::GpuMutex()`.
  - `agc_driver_draw_pipeline_preparation`:
    - **Red** (when `PrepareDrawPipelines` is a no-op): fails with `AGC graphics: no pipeline library parts were built before the draw`.
    - **Green**: passes in `0.58s` (`draw pipeline preparation tests passed`).
  - `agc_driver_graphics` (`pipelineLibraryConcurrencyTests` & `cachedPipelineConcurrencyTests`):
    - **Red**: fails when `vkCreateGraphicsPipelines` is serialized under `PipelineLibraryCache::mutex` or `Impl::mutex` (`expected concurrent vkCreateGraphicsPipelines calls while building independent pipeline libraries` / `expected the cached pipeline lookup to finish while the new pipeline was blocked in vkCreateGraphicsPipelines`).
    - **Green**: passes in `6.45s` (`Graphics validation tests passed`).
  - `agc_driver_dcc_key_presync`:
    - **Green**: passes in `0.60s` (`DCC key pre-sync tests passed`).
  - `agc_driver_present_lock_wait`:
    - Executed via CTest (`0.46s`, gracefully reports `skipped, the device has no VK_EXT_headless_surface for a presentation window` on headless RTX 5090 test environment).
- **Conventions & Test Registration Checks**:
  - `python tools/check_conventions.py --base refs/remotes/pr/7643`: `All conventions checks passed.`
  - `python tools/check_test_registration.py build`: `1002 test executable(s), 0 built and not run as recorded, none new`


#### Performance report

##### 1. PR onto `main` (`origin/main` `3fdd1f52e968` vs `arch/d-unlock-pipeline-and-present-clean` `3c9517994bfa`)

Pre-building graphics pipelines outside `GuestMemory::GpuMutex()` (`PrepareDrawPipelines`), per-key in-flight synchronization in `PipelineLibraryCache` and `Pipeline`, and cached `DccKey` reuse across unlocked pre-sync reduce `Driver.Packet` CPU time by **6.3%** (`14.175` -> `13.288` ms/frame) and `p99` frame time by **4.3%** (`17.643` -> `16.876` ms).

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`3fdd1f52e968` -> `3c9517994bfa`)**:
  - `frame_ms`: p50 `16.677` -> `16.677` ms | p90 `17.288` -> `16.787` ms | p99 `17.643` -> `16.876` ms | max `17.643` -> `16.876` ms
  - `gpu_busy_ms_per_present`: `5.026` -> `5.198` ms (`3.4%`)
  - `Driver.Packet` (ms/frame): `14.175` -> `13.288` ms (`-6.3%`)
- **Attached Traces**:
  - Baseline (`3fdd1f52e968`): `U:/scratch/anyps5/traces/origin-main/aps5-trace-3fdd1f52e968-121052.json.gz` (summary: `U:/scratch/anyps5/traces/origin-main/aps5-summary.jsonl`)
  - Change (`3c9517994bfa`): `U:/scratch/anyps5/traces/d-unlock-pipeline-and-present-clean/aps5-trace-3c9517994bfa-65236.json.gz` (summary: `U:/scratch/anyps5/traces/d-unlock-pipeline-and-present-clean/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`3fdd1f52e968` vs `3c9517994bfa`)**:
```json
{"commit":"3fdd1f52e968","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":326.986},"presents":20,"frame_ms":{"p50":16.677,"p90":17.288,"p99":17.643,"max":17.643},"gpu_busy_ms_per_present":5.026,"worker_busy_pct":{"queue worker 0x20":0.935,"queue worker 0x21":0.831,"queue worker 0x0":85.718},"top_phases_ms_per_frame":{"Driver.Packet":14.175,"Driver.Packet.draw":12.722,"Driver.Draw":12.539,"Driver.Draw.draw_enqueued":8.822,"Vulkan.DrawFromRecipe":7.757,"Graphics.DrawWithRecipe":7.696,"Graphics.DrawWithRecipe.draw_and_resource_release":7.674,"Graphics.Draw":4.146},"trace_dropped":false,"trace":"aps5-trace-3fdd1f52e968-121052.json.gz"}
{"commit":"3c9517994bfa","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":324.449},"presents":20,"frame_ms":{"p50":16.677,"p90":16.787,"p99":16.876,"max":16.876},"gpu_busy_ms_per_present":5.198,"worker_busy_pct":{"queue worker 0x20":1.022,"queue worker 0x21":1.281,"queue worker 0x0":80.536},"top_phases_ms_per_frame":{"Driver.Packet":13.288,"Driver.Packet.draw":11.722,"Driver.Draw":11.546,"Driver.Draw.draw_enqueued":8.160,"Vulkan.DrawFromRecipe":7.291,"Graphics.DrawWithRecipe":7.232,"Graphics.DrawWithRecipe.draw_and_resource_release":7.211,"Recorder.Sync":3.943},"trace_dropped":false,"trace":"aps5-trace-3c9517994bfa-65236.json.gz"}
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
**`python tools/trace_digest.py` — With Change (`3c9517994bfa`, `aps5-trace-3c9517994bfa-65236.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.8 / p99 16.9 / max 16.9 ms, GPU busy 5.2 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 3c9517994bfa, dropped: no

```json
{"commit":"3c9517994bfa","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":324.449},"presents":20,"frame_ms":{"p50":16.677,"p90":16.787,"p99":16.876,"max":16.876},"gpu_busy_ms_per_present":5.198,"worker_busy_pct":{"queue worker 0x20":1.022,"queue worker 0x21":1.281,"queue worker 0x0":80.536},"top_phases_ms_per_frame":{"Driver.Packet":13.288,"Driver.Packet.draw":11.722,"Driver.Draw":11.546,"Driver.Draw.draw_enqueued":8.16,"Vulkan.DrawFromRecipe":7.291,"Graphics.DrawWithRecipe":7.232,"Graphics.DrawWithRecipe.draw_and_resource_release":7.211,"Recorder.Sync":3.943},"trace_dropped":false,"trace":"aps5-trace-3c9517994bfa-65236.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 17: 16.9 ms at 191 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 22: 16.8 ms at 274 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 21: 16.8 ms at 258 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 15: 16.7 ms at 158 ms: Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6, Driver.WorkerWait (queue worker 0x21) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.5
- present 12: 16.7 ms at 108 ms: Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6, Driver.WorkerWait (queue worker 0x21) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.5

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.3 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.0 |
| queue worker 0x21 | 0.1 (1 %) | 15.3 (94 %) | 0.9 | 0.1 | Driver.EndSubmission 0.1, Driver.Packet 0.1 |
| queue worker 0x0 | 7.6 (47 %) | 2.7 (17 %) | 6.0 | 21.2 | Driver.Packet 7.6 |
| draw committer | 6.4 (39 %) | 0.0 (0 %) | 9.8 | 10.0 | Vulkan.DrawFromRecipe 6.1, Graphics.Draw 0.2 |
| presenter | 0.7 (4 %) | 1.9 (12 %) | 13.6 | 0.1 | VideoOut.Flip 2.5, Driver.Present 0.0 |
| gpu | 5.2 (32 %) | 0.0 (0 %) | 11.0 | 0.0 | batch 5.2 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 33.1, Driver.WorkerWait.idle_and_completion_poll 33.1, Driver.Packet 7.7, Driver.Packet.draw 7.1, Driver.Draw 7.0, Driver.Draw.draw_enqueued 6.2, Vulkan.DrawFromRecipe 6.1, Graphics.DrawWithRecipe 6.1, Graphics.DrawWithRecipe.draw_and_resource_release 6.1, batch 5.2

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 159 ms, present 15
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 75 ms, present 10
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 192 ms, present 17
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 259 ms, present 21
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 75 ms, present 10
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 276 ms, present 22
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 259 ms, present 21
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 92 ms, present 11
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 159 ms, present 15
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 276 ms, present 22

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 234: 1.0 ms at 57 ms, present 8: GPU no ranges; recorded by Recorder.Submit (draw committer) 0.0, Recorder.Reap (draw committer) 0.0
- batch 405: 0.6 ms at 100 ms, present 11: GPU no ranges; recorded by Vulkan.DrawFromRecipe (draw committer) 0.1, Graphics.DrawWithRecipe (draw committer) 0.1, Graphics.DrawWithRecipe.draw_and_resource_release (draw committer) 0.1, Graphics.Draw (draw committer) 0.0
- batch 642: 0.6 ms at 166 ms, present 15: GPU no ranges; recorded by Vulkan.DrawFromRecipe (draw committer) 0.2, Graphics.DrawWithRecipe (draw committer) 0.2, Graphics.DrawWithRecipe.draw_and_resource_release (draw committer) 0.2, Graphics.Draw (draw committer) 0.0
- batch 406: 0.6 ms at 100 ms, present 11: GPU no ranges; recorded by no CPU slices
- batch 582: 0.6 ms at 149 ms, present 14: GPU no ranges; recorded by Vulkan.DrawFromRecipe (draw committer) 0.1, Graphics.DrawWithRecipe (draw committer) 0.1, Graphics.DrawWithRecipe.draw_and_resource_release (draw committer) 0.1, Graphics.Draw (draw committer) 0.1
````
**`python tools/trace_digest.py` — Interval Summary (`3fdd1f52e968` vs `3c9517994bfa`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
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
# With Change (3c9517994bfa)
**3 intervals**, 0.0 to 3.0 s, 179 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 3c9517994bfa

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.9 | 19.2 | 19.2 | 5.1 | 76 % | Driver.Packet 12.7 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 16.9 | 16.9 | 5.0 | 73 % | Driver.Packet 12.4 |
| 3 | 2-3 | 58 | 17.3 | 18.8 | 22.1 | 22.1 | 4.9 | 87 % | Driver.Packet 15.4 |

```
p99 ms  %#@  0-22.1
gpu ms  @@@  0-5.1
```
````

---

##### 2. Stacked on `#7643` (`refs/remotes/pr/7643` `870ad5d6e747` vs `726dfe124c4c` — `Depends on: #7643`)

Stacked directly on `#7643` (`870ad5d6e747` -> `726dfe124c4c`), `gpu_busy_ms_per_present` improves by **5.0%** (`6.114` -> `5.811` ms) and `queue worker 0x0` busy percentage drops from `62.215%` to `61.543%`.

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`870ad5d6e747` -> `726dfe124c4c`)**:
  - `frame_ms`: p50 `16.682` -> `16.658` ms | p90 `16.808` -> `16.885` ms | p99 `17.529` -> `17.625` ms | max `17.529` -> `17.625` ms
  - `gpu_busy_ms_per_present`: `6.114` -> `5.811` ms (`-5.0%`)
  - `Driver.Packet` (ms/frame): `9.442` -> `10.453` ms (`10.7%`)
- **Attached Traces**:
  - Baseline (`870ad5d6e747`): `U:/scratch/anyps5/traces/pr-7643/aps5-trace-870ad5d6e747-21048.json.gz` (summary: `U:/scratch/anyps5/traces/pr-7643/aps5-summary.jsonl`)
  - Change (`726dfe124c4c`): `U:/scratch/anyps5/traces/d-unlock-pipeline-and-present-on-7643/aps5-trace-726dfe124c4c-86056.json.gz` (summary: `U:/scratch/anyps5/traces/d-unlock-pipeline-and-present-on-7643/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`870ad5d6e747` vs `726dfe124c4c`)**:
```json
{"commit":"870ad5d6e747","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.671},"presents":20,"frame_ms":{"p50":16.682,"p90":16.808,"p99":17.529,"max":17.529},"gpu_busy_ms_per_present":6.114,"worker_busy_pct":{"queue worker 0x20":0.939,"queue worker 0x21":1.120,"queue worker 0x0":55.436},"top_phases_ms_per_frame":{"Driver.Packet":9.442,"Driver.Packet.draw":7.972,"Driver.Draw":7.800,"VideoOut.Flip":6.500,"VideoOut.Flip.vblank_wait":5.685,"Driver.Draw.draw_enqueued":3.814,"Driver.Draw.shader_compile_and_link":1.308,"Driver.Draw.draw_and_resource_release":1.294},"trace_dropped":false,"trace":"aps5-trace-870ad5d6e747-21048.json.gz"}
{"commit":"726dfe124c4c","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.556},"presents":20,"frame_ms":{"p50":16.658,"p90":16.885,"p99":17.625,"max":17.625},"gpu_busy_ms_per_present":5.811,"worker_busy_pct":{"queue worker 0x20":0.998,"queue worker 0x21":0.968,"queue worker 0x0":61.543},"top_phases_ms_per_frame":{"Driver.Packet":10.453,"Driver.Packet.draw":8.817,"Driver.Draw":8.577,"VideoOut.Flip":5.239,"VideoOut.Flip.vblank_wait":4.129,"Driver.Draw.draw_enqueued":3.749,"Driver.Draw.shader_compile_and_link":1.593,"Driver.Draw.draw_and_resource_release":1.457},"trace_dropped":false,"trace":"aps5-trace-726dfe124c4c-86056.json.gz"}
```
**`python tools/trace_digest.py` — Baseline (`870ad5d6e747`, `aps5-trace-870ad5d6e747-21048.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.8 / p99 17.5 / max 17.5 ms, GPU busy 6.1 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 870ad5d6e747, dropped: no

```json
{"commit":"870ad5d6e747","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.671},"presents":20,"frame_ms":{"p50":16.682,"p90":16.808,"p99":17.529,"max":17.529},"gpu_busy_ms_per_present":6.114,"worker_busy_pct":{"queue worker 0x20":0.939,"queue worker 0x21":1.12,"queue worker 0x0":55.436},"top_phases_ms_per_frame":{"Driver.Packet":9.442,"Driver.Packet.draw":7.972,"Driver.Draw":7.8,"VideoOut.Flip":6.5,"VideoOut.Flip.vblank_wait":5.685,"Driver.Draw.draw_enqueued":3.814,"Driver.Draw.shader_compile_and_link":1.308,"Driver.Draw.draw_and_resource_release":1.294},"trace_dropped":false,"trace":"aps5-trace-870ad5d6e747-21048.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 17: 17.5 ms at 200 ms: Driver.WorkerWait (queue worker 0x20) 17.4, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 17.4, Driver.WorkerWait (queue worker 0x21) 17.3, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 17.2
- present 8: 16.8 ms at 50 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 23: 16.8 ms at 300 ms: Driver.WorkerWait (queue worker 0x20) 16.4, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.4, Driver.WorkerWait (queue worker 0x21) 16.4, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.4
- present 15: 16.8 ms at 167 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 9: 16.7 ms at 67 ms: Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x21 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.1 | Driver.EndSubmission 0.1, Driver.Packet 0.1 |
| queue worker 0x0 | 3.9 (24 %) | 6.7 (40 %) | 6.0 | 21.3 | Driver.Packet 3.9 |
| presenter | 0.7 (4 %) | 6.2 (37 %) | 9.8 | 0.1 | VideoOut.Flip 6.5, Driver.Present 0.1 |
| gpu | 6.2 (37 %) | 0.0 (0 %) | 10.5 | 0.0 | batch 6.2 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 38.1, Driver.WorkerWait.idle_and_completion_poll 38.1, VideoOut.Flip 6.5, batch 6.2, VideoOut.Flip.vblank_wait 5.7, Driver.Packet 4.1, Driver.Packet.draw 3.4, Driver.Draw 3.3, Driver.Draw.draw_enqueued 1.6, Driver.Present 1.1

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 17.5 ms at 202 ms, present 17
- Driver.WorkerWait (queue worker 0x21) 17.3 ms at 202 ms, present 17
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 51 ms, present 8
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 168 ms, present 15
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 52 ms, present 8
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 118 ms, present 12
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 68 ms, present 9
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 168 ms, present 15
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 35 ms, present 7
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 118 ms, present 12

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 276: 1.1 ms at 325 ms, present 24: GPU no ranges; recorded by no CPU slices
- batch 273: 1.0 ms at 323 ms, present 24: GPU no ranges; recorded by no CPU slices
- batch 218: 0.9 ms at 257 ms, present 20: GPU no ranges; recorded by no CPU slices
- batch 175: 0.9 ms at 206 ms, present 17: GPU no ranges; recorded by no CPU slices
- batch 192: 0.9 ms at 226 ms, present 18: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — With Change (`726dfe124c4c`, `aps5-trace-726dfe124c4c-86056.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.9 / p99 17.6 / max 17.6 ms, GPU busy 5.8 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 726dfe124c4c, dropped: no

```json
{"commit":"726dfe124c4c","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.556},"presents":20,"frame_ms":{"p50":16.658,"p90":16.885,"p99":17.625,"max":17.625},"gpu_busy_ms_per_present":5.811,"worker_busy_pct":{"queue worker 0x20":0.998,"queue worker 0x21":0.968,"queue worker 0x0":61.543},"top_phases_ms_per_frame":{"Driver.Packet":10.453,"Driver.Packet.draw":8.817,"Driver.Draw":8.577,"VideoOut.Flip":5.239,"VideoOut.Flip.vblank_wait":4.129,"Driver.Draw.draw_enqueued":3.749,"Driver.Draw.shader_compile_and_link":1.593,"Driver.Draw.draw_and_resource_release":1.457},"trace_dropped":false,"trace":"aps5-trace-726dfe124c4c-86056.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 10: 17.6 ms at 83 ms: Driver.WorkerWait (queue worker 0x20) 17.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 17.5, Driver.WorkerWait (queue worker 0x21) 17.4, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 17.4
- present 17: 16.9 ms at 200 ms: Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7, Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7
- present 21: 16.8 ms at 267 ms: Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7, Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6
- present 5: 16.8 ms at 0 ms: Driver.WorkerWait (queue worker 0x21) 14.9, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 14.9, Driver.WorkerWait (queue worker 0x20) 14.9, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 14.9
- present 20: 16.7 ms at 250 ms: Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6, Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.2 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x21 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.2 | Driver.EndSubmission 0.1, Driver.Packet 0.1 |
| queue worker 0x0 | 5.1 (31 %) | 5.6 (34 %) | 5.9 | 20.5 | Driver.Packet 5.1 |
| presenter | 1.0 (6 %) | 4.6 (28 %) | 11.1 | 0.1 | VideoOut.Flip 5.2, Driver.Present 0.1 |
| gpu | 5.8 (35 %) | 0.0 (0 %) | 10.8 | 0.0 | batch 5.8 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 37.0, Driver.WorkerWait.idle_and_completion_poll 37.0, batch 5.8, Driver.Packet 5.3, VideoOut.Flip 5.2, Driver.Packet.draw 4.6, Driver.Draw 4.3, VideoOut.Flip.vblank_wait 4.1, Driver.Draw.draw_enqueued 1.9, Driver.Present 1.1

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 19.7 ms at 168 ms, present 15
- Driver.WorkerWait (queue worker 0x21) 19.7 ms at 168 ms, present 15
- Driver.WorkerWait (queue worker 0x20) 17.6 ms at 85 ms, present 10
- Driver.WorkerWait (queue worker 0x21) 17.6 ms at 85 ms, present 10
- Driver.WorkerWait (queue worker 0x20) 17.3 ms at 51 ms, present 8
- Driver.WorkerWait (queue worker 0x21) 17.2 ms at 51 ms, present 8
- Driver.WorkerWait (queue worker 0x21) 16.9 ms at 251 ms, present 20
- Driver.WorkerWait (queue worker 0x21) 16.9 ms at 201 ms, present 17
- Driver.WorkerWait (queue worker 0x20) 16.9 ms at 251 ms, present 20
- Driver.WorkerWait (queue worker 0x20) 16.9 ms at 201 ms, present 17

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 46: 1.1 ms at 54 ms, present 8: GPU no ranges; recorded by no CPU slices
- batch 218: 1.0 ms at 256 ms, present 20: GPU no ranges; recorded by no CPU slices
- batch 274: 1.0 ms at 323 ms, present 24: GPU no ranges; recorded by no CPU slices
- batch 161: 0.9 ms at 193 ms, present 16: GPU no ranges; recorded by no CPU slices
- batch 277: 0.9 ms at 325 ms, present 24: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — Interval Summary (`870ad5d6e747` vs `726dfe124c4c`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
````markdown
# Baseline (870ad5d6e747)
**3 intervals**, 0.0 to 3.0 s, 181 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 870ad5d6e747

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.8 | 17.1 | 17.1 | 5.9 | 55 % | Driver.Packet 9.2 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 17.0 | 17.0 | 5.7 | 52 % | Driver.Packet 8.8 |
| 3 | 2-3 | 60 | 16.7 | 16.9 | 17.6 | 17.6 | 5.7 | 67 % | Driver.Packet 11.4 |

```
p99 ms  @@@  0-17.6
gpu ms  @@@  0-5.9
```
# With Change (726dfe124c4c)
**3 intervals**, 0.0 to 3.0 s, 181 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 726dfe124c4c

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.9 | 17.5 | 17.5 | 5.8 | 57 % | Driver.Packet 9.6 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 20.8 | 20.8 | 6.0 | 62 % | Driver.Packet 10.5 |
| 3 | 2-3 | 60 | 16.7 | 17.3 | 18.6 | 18.6 | 5.8 | 76 % | Driver.Packet 12.9 |

```
p99 ms  %@%  0-20.8
gpu ms  @@@  0-6.0
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
- [x] Depends on: #7643
- [x] Fixes: none
- [x] Unblocks: none
- [x] GPUs tested: NVIDIA GeForce RTX 5090 (driver 610.52, Windows 11)
- [x] AI-assisted: yes
