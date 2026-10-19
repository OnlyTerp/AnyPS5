Depends on: #6946, #6947, #7642, #7388

### What

- **Anchor `device_image_lookup` at the descriptor load (`PlanDeviceImageAnchors` & `EmitDeviceImageAnchors`)**:
  - Before emitting SPIR-V blocks, `PlanDeviceImageAnchors` finds the latest instruction in block order among the 8 `T#` descriptor dwords (`imageWords[0..7]`) and dynamic sampler dwords for each device-table image instruction.
  - Immediately after emitting that anchor instruction, `EmitDeviceImageAnchors` resolves each distinct `(imageWords, samplerClass, gatherComponent, isGather, signedSampled, integerSampled, array, msaa)` descriptor signature once via `EmitDeviceImageLookupValues`, storing the resulting `DeviceImageLookupValues` (`LookupWords`, `LookupMoreWords`, `LookupSamplerClass`, `LookupDword3`, `LookupActive`, `LookupFaulted`, `LookupNeedRecord`, `LookupElement`, `LookupSelector`) in `state.deviceImageLookups`.
  - `SpirvFlowEmitter` carries all 9 `DeviceImageLookupValues` entries through structured loop/merge `Phi` nodes (`ValueCount = 1 + LookupValueCount`) so samples inside downstream basic blocks and divergent branches read the resolved `slot` and `mode` directly without re-probing the device table.
- **Subgroup first-lane broadcast for wave-uniform descriptors (`WaveUniformLookup`)**:
  - When `state.sharedLaneValues` (populated via `WaveUniformValues` for compute and graphics stages) proves all 8 `T#` dwords and the sampler class are wave-uniform and the anchor block is not lane-divergent (`!ctx.divergent`), `EmitDeviceImageAnchors` elects the first active lane (`OpGroupNonUniformElect`), calls `device_image_lookup(words, moreWords, extra)` once on that lane, and broadcasts the `uvec4(slot, mode, fault, hit)` result across the subgroup with `OpGroupNonUniformBroadcastFirst`.
- **Sample-site miss/fault recording (`device_image_record_miss`)**:
  - Splits `DefineDeviceImageLookup` into pure lookup (`device_image_lookup(uvec4 low, uvec4 high, uvec4 extra) -> uvec4(slot, mode, fault, hit)`) when anchored, plus a dedicated `DontInline` helper `device_image_record_miss(uvec4 low, uvec4 high, uvec4 extra, bool faulted) -> void` sharing the ring-deduplication (`RingSeenWord`) and `appendRecord` logic.
  - At each sample site, `EmitDeviceTableAccess` reuses the anchored `LookupElement` and `LookupSelector` for the bindless heap sample and emits `device_image_record_miss` only when `active && needRecord` (gated by `ctx.execMask` on subgroup-1 paths), so unexecuted branch arms never pollute the miss ring.
- **Waterfall `ReadFirstLane` walkability & allocation-bounded pointer image tables (`#7388`)**:
  - Teaches `SrtInstructionPredicates::IsSrtWalkable`, `SrtEvaluator::EvaluateInstruction`, `SrtRuntimeValidator::AllPathsHold`, and `ResourceTracker` (`TraceWord` / `FollowScalarWord`) to walk through `IrOpcode::ReadFirstLane` and `IrOpcode::SelectU32`.
  - When a dynamic image table is rooted at a raw 64-bit pointer (`sourceBufferWords == 0`) rather than a `V#` buffer descriptor, `ResourceTracker` queries `GuestAllocations::AllocationCovering(baseAddress)` and bounds `dynamicTableEntries` by `(allocationEnd - baseAddress - addend) / stride` (capped at `MaxDynamicTableEntries`).

### Tested

- **Red / Green negative control**:
  - Temporarily bypassing `PlanDeviceImageAnchors` in `SpirvImageEmitter.cpp` caused `agc_shader_memory_tests --device-image-tables` to fail immediately (`device image tables: two samples of one wave-uniform descriptor did not emit one anchored lookup, one first-lane broadcast, and two miss records`), then passed once restored.
- **Unit & execution suites (`wt-shader`)**:
  - `agc_shader_memory_tests.exe --device-image-tables`: PASS
    - 2 samples of 1 wave-uniform `T#`: `1` `device_image_lookup` call, `1` `OpGroupNonUniformBroadcastFirst`, `2` `device_image_record_miss` calls (`10,641` SPIR-V words).
    - 3 samples of 1 wave-uniform `T#`: `1` `device_image_lookup` call, `1` `OpGroupNonUniformBroadcastFirst`, `3` `device_image_record_miss` calls (`12,454` SPIR-V words; delta `+1,813 <= 2,200` words).
    - 64 samples of 1 wave-uniform `T#`: `1` `device_image_lookup` call, `1` `OpGroupNonUniformBroadcastFirst`, `64` `device_image_record_miss` calls, `1` `DontInline` mode-switch helper called `64` times (`46,161 <= 60,000` SPIR-V words).
    - Descriptor loaded before a branch and sampled in one arm: `1` `device_image_lookup` call, `1` `OpGroupNonUniformBroadcastFirst`, `1` `device_image_record_miss` call.
  - `agc_shader_memory_tests.exe --srt-read-patching`: PASS (waterfall `v_readfirstlane_b32` + pointer image table bounded by enclosing guest allocation).
  - `agc_shader_memory_tests.exe --bindless`: PASS.
  - `agc_driver_device_image_table_tests.exe`: PASS on RTX 5090.
  - `agc_driver_device_image_path_tests.exe --village-bench` (`APS5_GPU_PATH=1`): PASS on RTX 5090 (`850` indirect draws/frame across `20` steady frames: `17,000` device table prepares, `0` CPU table snapshots, `0` CPU table bindings, `0.0` miss records/frame, `100.0%` expectations from memo at `1.14 us` each).


#### Performance report

##### 1. PR onto `main` (`origin/main` `3fdd1f52e968` vs `arch/a-resolve-descriptors-at-load-clean` `fe5808f59e76` — Device-Image Path `APS5_GPU_PATH=1`)

Anchoring `device_image_lookup` at the descriptor load and broadcasting wave-uniform lookups from the first active lane (`OpGroupNonUniformBroadcastFirst`) reduces `Driver.Packet` CPU time by **19.0%** (`15.349` -> `12.432` ms/frame) and `p99` frame time by **10.2%** (`18.740` -> `16.833` ms) under `APS5_GPU_PATH=1`.

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`3fdd1f52e968` -> `fe5808f59e76`)**:
  - `frame_ms`: p50 `16.898` -> `16.683` ms | p90 `18.484` -> `16.807` ms | p99 `18.74` -> `16.833` ms | max `18.74` -> `16.833` ms
  - `gpu_busy_ms_per_present`: `5.883` -> `6.236` ms (`6.0%`)
  - `Driver.Packet` (ms/frame): `15.349` -> `12.432` ms (`-19.0%`)
- **Attached Traces**:
  - Baseline (`3fdd1f52e968`): `U:/scratch/anyps5/traces/origin-main-gpu-path/aps5-trace-3fdd1f52e968-30076.json.gz` (summary: `U:/scratch/anyps5/traces/origin-main-gpu-path/aps5-summary.jsonl`)
  - Change (`fe5808f59e76`): `U:/scratch/anyps5/traces/a-resolve-descriptors-at-load-clean-gpu-path/aps5-trace-fe5808f59e76-33696.json.gz` (summary: `U:/scratch/anyps5/traces/a-resolve-descriptors-at-load-clean-gpu-path/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`3fdd1f52e968` vs `fe5808f59e76`)**:
```json
{"commit":"3fdd1f52e968","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":337.984},"presents":20,"frame_ms":{"p50":16.898,"p90":18.484,"p99":18.740,"max":18.740},"gpu_busy_ms_per_present":5.883,"worker_busy_pct":{"queue worker 0x21":1.178,"queue worker 0x20":1.061,"queue worker 0x0":89.704},"top_phases_ms_per_frame":{"Driver.Packet":15.349,"Driver.Packet.draw":13.743,"Driver.Draw":13.554,"Driver.Draw.draw_enqueued":9.524,"Vulkan.DrawFromRecipe":8.454,"Graphics.DrawWithRecipe":8.390,"Graphics.DrawWithRecipe.draw_and_resource_release":8.367,"Recorder.Sync":4.723},"trace_dropped":false,"trace":"aps5-trace-3fdd1f52e968-30076.json.gz"}
{"commit":"fe5808f59e76","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":328.485},"presents":20,"frame_ms":{"p50":16.683,"p90":16.807,"p99":16.833,"max":16.833},"gpu_busy_ms_per_present":6.236,"worker_busy_pct":{"queue worker 0x20":0.945,"queue worker 0x21":0.919,"queue worker 0x0":74.681},"top_phases_ms_per_frame":{"Driver.Packet":12.432,"Driver.Packet.draw":10.827,"Driver.Draw":10.706,"Driver.Draw.draw_enqueued":7.877,"Vulkan.DrawFromRecipe":7.067,"Graphics.DrawWithRecipe":7.007,"Graphics.DrawWithRecipe.draw_and_resource_release":6.987,"Recorder.Sync":3.867},"trace_dropped":false,"trace":"aps5-trace-fe5808f59e76-33696.json.gz"}
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
**`python tools/trace_digest.py` — With Change (`fe5808f59e76`, `aps5-trace-fe5808f59e76-33696.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.8 / p99 16.8 / max 16.8 ms, GPU busy 6.2 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit fe5808f59e76, dropped: no

```json
{"commit":"fe5808f59e76","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":328.485},"presents":20,"frame_ms":{"p50":16.683,"p90":16.807,"p99":16.833,"max":16.833},"gpu_busy_ms_per_present":6.236,"worker_busy_pct":{"queue worker 0x20":0.945,"queue worker 0x21":0.919,"queue worker 0x0":74.681},"top_phases_ms_per_frame":{"Driver.Packet":12.432,"Driver.Packet.draw":10.827,"Driver.Draw":10.706,"Driver.Draw.draw_enqueued":7.877,"Vulkan.DrawFromRecipe":7.067,"Graphics.DrawWithRecipe":7.007,"Graphics.DrawWithRecipe.draw_and_resource_release":6.987,"Recorder.Sync":3.867},"trace_dropped":false,"trace":"aps5-trace-fe5808f59e76-33696.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 15: 16.8 ms at 162 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 16: 16.8 ms at 178 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7
- present 12: 16.8 ms at 112 ms: Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6, Driver.WorkerWait (queue worker 0x20) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.5
- present 22: 16.8 ms at 278 ms: Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6, Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6
- present 18: 16.7 ms at 212 ms: Driver.WorkerWait (queue worker 0x21) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.5, Driver.WorkerWait (queue worker 0x20) 16.4, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.4

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.5 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x21 | 0.1 (1 %) | 15.5 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x0 | 7.4 (45 %) | 3.6 (22 %) | 5.4 | 17.6 | Driver.Packet 7.4, Driver.EndSubmission 0.0 |
| draw committer | 6.3 (38 %) | 0.0 (0 %) | 10.1 | 9.7 | Vulkan.DrawFromRecipe 5.9, Graphics.Draw 0.3 |
| presenter | 0.7 (4 %) | 2.9 (18 %) | 12.8 | 0.0 | VideoOut.Flip 3.4, Driver.Present 0.0 |
| gpu | 5.9 (36 %) | 0.0 (0 %) | 10.5 | 0.0 | batch 5.9 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 34.5, Driver.WorkerWait.idle_and_completion_poll 34.5, Driver.Packet 7.6, Driver.Packet.draw 6.8, Driver.Draw 6.8, Driver.Draw.draw_enqueued 6.1, batch 5.9, Vulkan.DrawFromRecipe 5.9, Graphics.DrawWithRecipe 5.9, Graphics.DrawWithRecipe.draw_and_resource_release 5.9

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 180 ms, present 16
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 63 ms, present 9
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 180 ms, present 16
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 280 ms, present 22
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 163 ms, present 15
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 96 ms, present 11
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 63 ms, present 9
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 280 ms, present 22
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 163 ms, present 15
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 96 ms, present 11

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 945: 1.0 ms at 256 ms, present 20: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.0, Graphics.Draw.draw_and_resource_release (draw committer) 0.0
- batch 826: 0.9 ms at 223 ms, present 18: GPU no ranges; recorded by no CPU slices
- batch 1064: 0.9 ms at 289 ms, present 22: GPU no ranges; recorded by no CPU slices
- batch 1003: 0.9 ms at 272 ms, present 21: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.0, Graphics.Draw.draw_and_resource_release (draw committer) 0.0, Recorder.Submit (draw committer) 0.0
- batch 1183: 0.9 ms at 323 ms, present 24: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — Interval Summary (`3fdd1f52e968` vs `fe5808f59e76`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
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
# With Change (fe5808f59e76)
**3 intervals**, 0.0 to 3.0 s, 181 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit fe5808f59e76

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.9 | 18.6 | 18.6 | 5.9 | 72 % | Driver.Packet 12.2 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 16.9 | 16.9 | 5.6 | 66 % | Driver.Packet 11.3 |
| 3 | 2-3 | 60 | 16.7 | 17.3 | 18.6 | 18.6 | 5.5 | 82 % | Driver.Packet 13.8 |

```
p99 ms  @%@  0-18.6
gpu ms  @@%  0-5.9
```
````

---

##### 2. PR onto `main` (`origin/main` `3fdd1f52e968` vs `arch/a-resolve-descriptors-at-load-clean` `fe5808f59e76` — Default Path)

On the default path, `Driver.Packet` CPU time drops by **12.2%** (`14.175` -> `12.439` ms/frame) and `p99` frame time drops from `17.643` to `17.047` ms.

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`3fdd1f52e968` -> `fe5808f59e76`)**:
  - `frame_ms`: p50 `16.677` -> `16.671` ms | p90 `17.288` -> `16.954` ms | p99 `17.643` -> `17.047` ms | max `17.643` -> `17.047` ms
  - `gpu_busy_ms_per_present`: `5.026` -> `5.69` ms (`13.2%`)
  - `Driver.Packet` (ms/frame): `14.175` -> `12.439` ms (`-12.2%`)
- **Attached Traces**:
  - Baseline (`3fdd1f52e968`): `U:/scratch/anyps5/traces/origin-main/aps5-trace-3fdd1f52e968-121052.json.gz` (summary: `U:/scratch/anyps5/traces/origin-main/aps5-summary.jsonl`)
  - Change (`fe5808f59e76`): `U:/scratch/anyps5/traces/a-resolve-descriptors-at-load-clean/aps5-trace-fe5808f59e76-66856.json.gz` (summary: `U:/scratch/anyps5/traces/a-resolve-descriptors-at-load-clean/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`3fdd1f52e968` vs `fe5808f59e76`)**:
```json
{"commit":"3fdd1f52e968","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":326.986},"presents":20,"frame_ms":{"p50":16.677,"p90":17.288,"p99":17.643,"max":17.643},"gpu_busy_ms_per_present":5.026,"worker_busy_pct":{"queue worker 0x20":0.935,"queue worker 0x21":0.831,"queue worker 0x0":85.718},"top_phases_ms_per_frame":{"Driver.Packet":14.175,"Driver.Packet.draw":12.722,"Driver.Draw":12.539,"Driver.Draw.draw_enqueued":8.822,"Vulkan.DrawFromRecipe":7.757,"Graphics.DrawWithRecipe":7.696,"Graphics.DrawWithRecipe.draw_and_resource_release":7.674,"Graphics.Draw":4.146},"trace_dropped":false,"trace":"aps5-trace-3fdd1f52e968-121052.json.gz"}
{"commit":"fe5808f59e76","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.620},"presents":20,"frame_ms":{"p50":16.671,"p90":16.954,"p99":17.047,"max":17.047},"gpu_busy_ms_per_present":5.690,"worker_busy_pct":{"queue worker 0x20":0.811,"queue worker 0x0":73.701,"queue worker 0x21":0.976},"top_phases_ms_per_frame":{"Driver.Packet":12.439,"Driver.Packet.draw":11.056,"Driver.Draw":10.950,"Driver.Draw.draw_enqueued":7.559,"Vulkan.DrawFromRecipe":6.880,"Graphics.DrawWithRecipe":6.823,"Graphics.DrawWithRecipe.draw_and_resource_release":6.804,"Graphics.Draw":3.941},"trace_dropped":false,"trace":"aps5-trace-fe5808f59e76-66856.json.gz"}
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
**`python tools/trace_digest.py` — With Change (`fe5808f59e76`, `aps5-trace-fe5808f59e76-66856.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 17.0 / p99 17.0 / max 17.0 ms, GPU busy 5.7 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit fe5808f59e76, dropped: no

```json
{"commit":"fe5808f59e76","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.62},"presents":20,"frame_ms":{"p50":16.671,"p90":16.954,"p99":17.047,"max":17.047},"gpu_busy_ms_per_present":5.69,"worker_busy_pct":{"queue worker 0x20":0.811,"queue worker 0x0":73.701,"queue worker 0x21":0.976},"top_phases_ms_per_frame":{"Driver.Packet":12.439,"Driver.Packet.draw":11.056,"Driver.Draw":10.95,"Driver.Draw.draw_enqueued":7.559,"Vulkan.DrawFromRecipe":6.88,"Graphics.DrawWithRecipe":6.823,"Graphics.DrawWithRecipe.draw_and_resource_release":6.804,"Graphics.Draw":3.941},"trace_dropped":false,"trace":"aps5-trace-fe5808f59e76-66856.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 16: 17.0 ms at 183 ms: Driver.WorkerWait (queue worker 0x20) 16.9, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.9, Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8
- present 19: 17.0 ms at 233 ms: Driver.WorkerWait (queue worker 0x20) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.8, Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8
- present 24: 16.9 ms at 317 ms: Driver.Packet (queue worker 0x0) 6.2, batch (gpu) 5.8, Driver.Packet.draw (queue worker 0x0) 5.7, Driver.Draw (queue worker 0x0) 5.6
- present 22: 16.8 ms at 283 ms: Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7, Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7
- present 10: 16.8 ms at 83 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.0 |
| queue worker 0x0 | 7.1 (42 %) | 3.8 (23 %) | 5.8 | 20.1 | Driver.Packet 7.0, Driver.EndSubmission 0.0 |
| queue worker 0x21 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.1 | Driver.EndSubmission 0.1, Driver.Packet 0.1 |
| draw committer | 6.1 (36 %) | 0.0 (0 %) | 10.6 | 9.7 | Vulkan.DrawFromRecipe 5.7, Graphics.Draw 0.2 |
| presenter | 0.7 (4 %) | 3.2 (19 %) | 12.8 | 0.1 | VideoOut.Flip 3.7, Driver.Present 0.1 |
| gpu | 5.7 (34 %) | 0.0 (0 %) | 11.0 | 0.0 | batch 5.7 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 35.2, Driver.WorkerWait.idle_and_completion_poll 35.2, Driver.Packet 7.2, Driver.Packet.draw 6.7, Driver.Draw 6.6, Driver.Draw.draw_enqueued 5.8, Vulkan.DrawFromRecipe 5.7, Graphics.DrawWithRecipe 5.7, Graphics.DrawWithRecipe.draw_and_resource_release 5.7, batch 5.7

**Longest waits**

- Driver.WorkerWait (queue worker 0x21) 16.9 ms at 284 ms, present 22
- Driver.WorkerWait (queue worker 0x20) 16.9 ms at 284 ms, present 22
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 168 ms, present 15
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 84 ms, present 10
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 235 ms, present 19
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 34 ms, present 7
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 185 ms, present 16
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 235 ms, present 19
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 168 ms, present 15
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 185 ms, present 16

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 1181: 1.0 ms at 327 ms, present 24: GPU no ranges; recorded by no CPU slices
- batch 54: 0.9 ms at 10 ms, present 5: GPU no ranges; recorded by GpuMutex.wait.draw (draw committer) 0.0
- batch 647: 0.9 ms at 176 ms, present 15: GPU no ranges; recorded by no CPU slices
- batch 530: 0.9 ms at 144 ms, present 13: GPU no ranges; recorded by no CPU slices
- batch 766: 0.9 ms at 210 ms, present 17: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — Interval Summary (`3fdd1f52e968` vs `fe5808f59e76`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
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
# With Change (fe5808f59e76)
**3 intervals**, 0.0 to 3.0 s, 181 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit fe5808f59e76

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.9 | 18.8 | 18.8 | 5.8 | 75 % | Driver.Packet 12.5 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 16.9 | 16.9 | 5.7 | 72 % | Driver.Packet 12.3 |
| 3 | 2-3 | 60 | 16.7 | 17.6 | 18.8 | 18.8 | 5.4 | 86 % | Driver.Packet 14.5 |

```
p99 ms  @%@  0-18.8
gpu ms  @@%  0-5.8
```
````

---

##### 3. Stacked on Lane A.1 (`arch/a-device-image-code-size-clean` `c23ffbbc59b6` vs `arch/a-resolve-descriptors-at-load-clean` `fe5808f59e76` — `APS5_GPU_PATH=1`)

Stacked on top of Lane A.1 (`arch/a-device-image-code-size-clean`), resolving wave-uniform descriptors once at load instead of per sample site further reduces `Driver.Packet` (`12.649` -> `12.432` ms) and `p99` frame time (`16.953` -> `16.833` ms).

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`c23ffbbc59b6` -> `fe5808f59e76`)**:
  - `frame_ms`: p50 `16.681` -> `16.683` ms | p90 `16.886` -> `16.807` ms | p99 `16.953` -> `16.833` ms | max `16.953` -> `16.833` ms
  - `gpu_busy_ms_per_present`: `6.065` -> `6.236` ms (`2.8%`)
  - `Driver.Packet` (ms/frame): `12.649` -> `12.432` ms (`-1.7%`)
- **Attached Traces**:
  - Baseline (`c23ffbbc59b6`): `U:/scratch/anyps5/traces/a-device-image-code-size-clean-gpu-path/aps5-trace-c23ffbbc59b6-126312.json.gz` (summary: `U:/scratch/anyps5/traces/a-device-image-code-size-clean-gpu-path/aps5-summary.jsonl`)
  - Change (`fe5808f59e76`): `U:/scratch/anyps5/traces/a-resolve-descriptors-at-load-clean-gpu-path/aps5-trace-fe5808f59e76-33696.json.gz` (summary: `U:/scratch/anyps5/traces/a-resolve-descriptors-at-load-clean-gpu-path/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`c23ffbbc59b6` vs `fe5808f59e76`)**:
```json
{"commit":"c23ffbbc59b6","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":327.730},"presents":20,"frame_ms":{"p50":16.681,"p90":16.886,"p99":16.953,"max":16.953},"gpu_busy_ms_per_present":6.065,"worker_busy_pct":{"queue worker 0x21":1.144,"queue worker 0x20":1.216,"queue worker 0x0":75.801},"top_phases_ms_per_frame":{"Driver.Packet":12.649,"Driver.Packet.draw":10.800,"Driver.Draw":10.688,"Driver.Draw.draw_enqueued":7.799,"Vulkan.DrawFromRecipe":7.270,"Graphics.DrawWithRecipe":7.212,"Graphics.DrawWithRecipe.draw_and_resource_release":7.191,"Recorder.Sync":3.842},"trace_dropped":false,"trace":"aps5-trace-c23ffbbc59b6-126312.json.gz"}
{"commit":"fe5808f59e76","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":328.485},"presents":20,"frame_ms":{"p50":16.683,"p90":16.807,"p99":16.833,"max":16.833},"gpu_busy_ms_per_present":6.236,"worker_busy_pct":{"queue worker 0x20":0.945,"queue worker 0x21":0.919,"queue worker 0x0":74.681},"top_phases_ms_per_frame":{"Driver.Packet":12.432,"Driver.Packet.draw":10.827,"Driver.Draw":10.706,"Driver.Draw.draw_enqueued":7.877,"Vulkan.DrawFromRecipe":7.067,"Graphics.DrawWithRecipe":7.007,"Graphics.DrawWithRecipe.draw_and_resource_release":6.987,"Recorder.Sync":3.867},"trace_dropped":false,"trace":"aps5-trace-fe5808f59e76-33696.json.gz"}
```
**`python tools/trace_digest.py` — Baseline (`c23ffbbc59b6`, `aps5-trace-c23ffbbc59b6-126312.json.gz`)**:
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
**`python tools/trace_digest.py` — With Change (`fe5808f59e76`, `aps5-trace-fe5808f59e76-33696.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.8 / p99 16.8 / max 16.8 ms, GPU busy 6.2 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit fe5808f59e76, dropped: no

```json
{"commit":"fe5808f59e76","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":328.485},"presents":20,"frame_ms":{"p50":16.683,"p90":16.807,"p99":16.833,"max":16.833},"gpu_busy_ms_per_present":6.236,"worker_busy_pct":{"queue worker 0x20":0.945,"queue worker 0x21":0.919,"queue worker 0x0":74.681},"top_phases_ms_per_frame":{"Driver.Packet":12.432,"Driver.Packet.draw":10.827,"Driver.Draw":10.706,"Driver.Draw.draw_enqueued":7.877,"Vulkan.DrawFromRecipe":7.067,"Graphics.DrawWithRecipe":7.007,"Graphics.DrawWithRecipe.draw_and_resource_release":6.987,"Recorder.Sync":3.867},"trace_dropped":false,"trace":"aps5-trace-fe5808f59e76-33696.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 15: 16.8 ms at 162 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 16: 16.8 ms at 178 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.7
- present 12: 16.8 ms at 112 ms: Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6, Driver.WorkerWait (queue worker 0x20) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.5
- present 22: 16.8 ms at 278 ms: Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6, Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6
- present 18: 16.7 ms at 212 ms: Driver.WorkerWait (queue worker 0x21) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.5, Driver.WorkerWait (queue worker 0x20) 16.4, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.4

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.5 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x21 | 0.1 (1 %) | 15.5 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x0 | 7.4 (45 %) | 3.6 (22 %) | 5.4 | 17.6 | Driver.Packet 7.4, Driver.EndSubmission 0.0 |
| draw committer | 6.3 (38 %) | 0.0 (0 %) | 10.1 | 9.7 | Vulkan.DrawFromRecipe 5.9, Graphics.Draw 0.3 |
| presenter | 0.7 (4 %) | 2.9 (18 %) | 12.8 | 0.0 | VideoOut.Flip 3.4, Driver.Present 0.0 |
| gpu | 5.9 (36 %) | 0.0 (0 %) | 10.5 | 0.0 | batch 5.9 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 34.5, Driver.WorkerWait.idle_and_completion_poll 34.5, Driver.Packet 7.6, Driver.Packet.draw 6.8, Driver.Draw 6.8, Driver.Draw.draw_enqueued 6.1, batch 5.9, Vulkan.DrawFromRecipe 5.9, Graphics.DrawWithRecipe 5.9, Graphics.DrawWithRecipe.draw_and_resource_release 5.9

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 180 ms, present 16
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 63 ms, present 9
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 180 ms, present 16
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 280 ms, present 22
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 163 ms, present 15
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 96 ms, present 11
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 63 ms, present 9
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 280 ms, present 22
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 163 ms, present 15
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 96 ms, present 11

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 945: 1.0 ms at 256 ms, present 20: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.0, Graphics.Draw.draw_and_resource_release (draw committer) 0.0
- batch 826: 0.9 ms at 223 ms, present 18: GPU no ranges; recorded by no CPU slices
- batch 1064: 0.9 ms at 289 ms, present 22: GPU no ranges; recorded by no CPU slices
- batch 1003: 0.9 ms at 272 ms, present 21: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.0, Graphics.Draw.draw_and_resource_release (draw committer) 0.0, Recorder.Submit (draw committer) 0.0
- batch 1183: 0.9 ms at 323 ms, present 24: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — Interval Summary (`c23ffbbc59b6` vs `fe5808f59e76`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
````markdown
# Baseline (c23ffbbc59b6)
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
# With Change (fe5808f59e76)
**3 intervals**, 0.0 to 3.0 s, 181 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit fe5808f59e76

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.9 | 18.6 | 18.6 | 5.9 | 72 % | Driver.Packet 12.2 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 16.9 | 16.9 | 5.6 | 66 % | Driver.Packet 11.3 |
| 3 | 2-3 | 60 | 16.7 | 17.3 | 18.6 | 18.6 | 5.5 | 82 % | Driver.Packet 13.8 |

```
p99 ms  @%@  0-18.6
gpu ms  @@%  0-5.9
```
````

---

##### 4. Stacked on `#7642` + Lane A.1 (`43b7a6d8da5c` vs `80adbf4f1eb3` — `APS5_GPU_PATH=1`)

Stacked on `#7642` + Lane A.1 (`43b7a6d8da5c` -> `80adbf4f1eb3`) with `APS5_GPU_PATH=1`: `Driver.Packet` drops by **5.9%** (`9.169` -> `8.632` ms/frame) and `p99` drops from `16.993` to `16.944` ms.

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`43b7a6d8da5c` -> `80adbf4f1eb3`)**:
  - `frame_ms`: p50 `16.689` -> `16.682` ms | p90 `16.873` -> `16.926` ms | p99 `16.993` -> `16.944` ms | max `16.993` -> `16.944` ms
  - `gpu_busy_ms_per_present`: `5.826` -> `6.177` ms (`6.0%`)
  - `Driver.Packet` (ms/frame): `9.169` -> `8.632` ms (`-5.9%`)
- **Attached Traces**:
  - Baseline (`43b7a6d8da5c`): `U:/scratch/anyps5/traces/a-device-image-code-size-on-7642-gpu-path/aps5-trace-43b7a6d8da5c-127748.json.gz` (summary: `U:/scratch/anyps5/traces/a-device-image-code-size-on-7642-gpu-path/aps5-summary.jsonl`)
  - Change (`80adbf4f1eb3`): `U:/scratch/anyps5/traces/a-resolve-descriptors-at-load-on-7642-gpu-path/aps5-trace-80adbf4f1eb3-60036.json.gz` (summary: `U:/scratch/anyps5/traces/a-resolve-descriptors-at-load-on-7642-gpu-path/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`43b7a6d8da5c` vs `80adbf4f1eb3`)**:
```json
{"commit":"43b7a6d8da5c","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.529},"presents":20,"frame_ms":{"p50":16.689,"p90":16.873,"p99":16.993,"max":16.993},"gpu_busy_ms_per_present":5.826,"worker_busy_pct":{"queue worker 0x20":0.942,"queue worker 0x21":1.058,"queue worker 0x0":53.871},"top_phases_ms_per_frame":{"Driver.Packet":9.169,"Driver.Packet.draw":7.620,"Driver.Draw":7.490,"VideoOut.Flip":6.581,"VideoOut.Flip.vblank_wait":5.618,"Driver.Draw.draw_enqueued":3.706,"Driver.Draw.shader_compile_and_link":1.566,"Driver.Present":1.120},"trace_dropped":false,"trace":"aps5-trace-43b7a6d8da5c-127748.json.gz"}
{"commit":"80adbf4f1eb3","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.666},"presents":20,"frame_ms":{"p50":16.682,"p90":16.926,"p99":16.944,"max":16.944},"gpu_busy_ms_per_present":6.177,"worker_busy_pct":{"queue worker 0x20":1.166,"queue worker 0x21":0.927,"queue worker 0x0":50.740},"top_phases_ms_per_frame":{"Driver.Packet":8.632,"VideoOut.Flip":7.291,"Driver.Packet.draw":7.284,"Driver.Draw":7.166,"VideoOut.Flip.vblank_wait":6.494,"Driver.Draw.draw_enqueued":3.898,"Driver.Draw.shader_compile_and_link":1.356,"Driver.Present":1.101},"trace_dropped":false,"trace":"aps5-trace-80adbf4f1eb3-60036.json.gz"}
```
**`python tools/trace_digest.py` — Baseline (`43b7a6d8da5c`, `aps5-trace-43b7a6d8da5c-127748.json.gz`)**:
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
**`python tools/trace_digest.py` — With Change (`80adbf4f1eb3`, `aps5-trace-80adbf4f1eb3-60036.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.9 / p99 16.9 / max 16.9 ms, GPU busy 6.2 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 80adbf4f1eb3, dropped: no

```json
{"commit":"80adbf4f1eb3","dirty":false,"env":{"APS5_GPU_PATH":"1","APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.666},"presents":20,"frame_ms":{"p50":16.682,"p90":16.926,"p99":16.944,"max":16.944},"gpu_busy_ms_per_present":6.177,"worker_busy_pct":{"queue worker 0x20":1.166,"queue worker 0x21":0.927,"queue worker 0x0":50.74},"top_phases_ms_per_frame":{"Driver.Packet":8.632,"VideoOut.Flip":7.291,"Driver.Packet.draw":7.284,"Driver.Draw":7.166,"VideoOut.Flip.vblank_wait":6.494,"Driver.Draw.draw_enqueued":3.898,"Driver.Draw.shader_compile_and_link":1.356,"Driver.Present":1.101},"trace_dropped":false,"trace":"aps5-trace-80adbf4f1eb3-60036.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 18: 16.9 ms at 217 ms: Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8, Driver.WorkerWait (queue worker 0x20) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.8
- present 21: 16.9 ms at 267 ms: Driver.WorkerWait (queue worker 0x20) 16.8, Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.8
- present 7: 16.7 ms at 33 ms: Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 14: 16.7 ms at 150 ms: Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6, Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6
- present 10: 16.7 ms at 83 ms: Driver.WorkerWait (queue worker 0x20) 16.3, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.3, Driver.WorkerWait (queue worker 0x21) 16.3, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.3

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.1 | Driver.EndSubmission 0.1, Driver.Packet 0.1 |
| queue worker 0x21 | 0.1 (1 %) | 15.7 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x0 | 3.0 (18 %) | 7.5 (45 %) | 6.2 | 20.3 | Driver.Packet 3.0, Driver.EndSubmission 0.0 |
| presenter | 0.6 (4 %) | 7.0 (42 %) | 9.0 | 0.1 | VideoOut.Flip 7.3, Driver.Present 0.1 |
| gpu | 6.3 (37 %) | 0.0 (0 %) | 10.4 | 0.0 | batch 6.3 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 38.9, Driver.WorkerWait.idle_and_completion_poll 38.9, VideoOut.Flip 7.3, VideoOut.Flip.vblank_wait 6.5, batch 6.3, Driver.Packet 3.1, Driver.Packet.draw 2.6, Driver.Draw 2.6, Driver.Draw.draw_enqueued 1.7, Driver.Present 1.1

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 268 ms, present 21
- Driver.WorkerWait (queue worker 0x21) 16.8 ms at 268 ms, present 21
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 35 ms, present 7
- Driver.WorkerWait (queue worker 0x21) 16.8 ms at 35 ms, present 7
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 168 ms, present 15
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 218 ms, present 18
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 202 ms, present 17
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 168 ms, present 15
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 118 ms, present 12
- Driver.WorkerWait (queue worker 0x20) 16.7 ms at 218 ms, present 18

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 145: 1.4 ms at 172 ms, present 15: GPU no ranges; recorded by no CPU slices
- batch 187: 1.3 ms at 221 ms, present 18: GPU no ranges; recorded by no CPU slices
- batch 276: 1.1 ms at 325 ms, present 24: GPU no ranges; recorded by no CPU slices
- batch 163: 1.1 ms at 192 ms, present 16: GPU no ranges; recorded by no CPU slices
- batch 147: 1.1 ms at 174 ms, present 15: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — Interval Summary (`43b7a6d8da5c` vs `80adbf4f1eb3`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
````markdown
# Baseline (43b7a6d8da5c)
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
# With Change (80adbf4f1eb3)
**3 intervals**, 0.0 to 3.0 s, 181 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit 80adbf4f1eb3

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.9 | 17.0 | 17.0 | 6.5 | 46 % | VideoOut.Flip 8.1 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 16.9 | 16.9 | 6.4 | 53 % | Driver.Packet 9.0 |
| 3 | 2-3 | 60 | 16.7 | 16.8 | 16.9 | 16.9 | 6.1 | 60 % | Driver.Packet 10.3 |

```
p99 ms  @@@  0-17.0
gpu ms  @@%  0-6.5
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
- [x] Depends on: #6946, #6947, #7642, #7388
- [x] Fixes: none
- [x] Unblocks: PPSA01556
- [x] GPUs tested: NVIDIA GeForce RTX 5090 (driver 610.52, Windows 11)
- [x] AI-assisted: yes
