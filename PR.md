### What

- Add `Detail::Evaluator::Reads` (`EvaluatorReads::Nothing`, `Arguments`, `Closure`) and prune `ownPlanValues` under control-flow branch conditions so resource plans retain only the IR values that runtime condition evaluation can actually read, keeping unevaluable operations (such as vector buffer loads) as 0-argument leaves and opening only the descriptor handle and offset of scalar raw reads instead of copying their transitive vector closure.
- Preserve compute `workgroupThreads` in `IrResourcePlan` from `InstructionTranslator::Translate` through `ResourceMaterializer::ExtractPlan` and the shader disk cache plan codec (`FormatVersion` 41), and reserve Phi predecessor block capacity when decoding cached plans without use lists.

### Tested

| Case | Red (before fix) | Green (with fix) |
| --- | --- | --- |
| `agc_shader_memory` (`verifyPlanConditionPruning`: 64-step `IAdd32` chain under `LoadBufferU32` in an extracted pair condition, plus 1/16/64-step vector chains before a scalar `LoadAddressU32` condition) | Fails with `plan pruning: the address computation under a load the evaluator cannot read was copied` (and retains 15, 30, 78 values for chains of 1, 16, 64 steps) | Passes in 2.48 s (`0` `IAdd32` values copied, `LoadBufferU32` kept as a 0-argument leaf, identical 13 retained values for chains of 1, 16, and 64 steps, 0 use lists or spare operand capacity) |
| `agc_shader_disk_cache` (`--resource-plan-codec` and full suite) | Fails with `the decoded hand-built plan differs from the plan in a field or a value` when `workgroupThreads` is omitted from the codec | Passes in 3.35 s (17 plan entries round-trip and compare equal under `samePlan` including `workgroupThreads = {64, 2, 3}`) |
| `agc_ir_reclaim` | Passes | Passes in 0.01 s |


#### Performance report

##### 1. PR onto `main` (`origin/main` `3fdd1f52e968` vs `arch/b-plan-retention-clean` `db42fda731b2`)

Pruning `ownPlanValues` via `Detail::Evaluator::Reads` and preserving `workgroupThreads` across shader disk cache serialization (`FormatVersion` 41) improves `p90` (`17.288` -> `17.080` ms) and `p99` (`17.643` -> `17.213` ms) frame times.

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`3fdd1f52e968` -> `db42fda731b2`)**:
  - `frame_ms`: p50 `16.677` -> `16.684` ms | p90 `17.288` -> `17.08` ms | p99 `17.643` -> `17.213` ms | max `17.643` -> `17.213` ms
  - `gpu_busy_ms_per_present`: `5.026` -> `6.057` ms (`20.5%`)
  - `Driver.Packet` (ms/frame): `14.175` -> `14.265` ms (`0.6%`)
- **Attached Traces**:
  - Baseline (`3fdd1f52e968`): `U:/scratch/anyps5/traces/origin-main/aps5-trace-3fdd1f52e968-121052.json.gz` (summary: `U:/scratch/anyps5/traces/origin-main/aps5-summary.jsonl`)
  - Change (`db42fda731b2`): `U:/scratch/anyps5/traces/b-plan-retention-clean/aps5-trace-db42fda731b2-93052.json.gz` (summary: `U:/scratch/anyps5/traces/b-plan-retention-clean/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`3fdd1f52e968` vs `db42fda731b2`)**:
```json
{"commit":"3fdd1f52e968","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":326.986},"presents":20,"frame_ms":{"p50":16.677,"p90":17.288,"p99":17.643,"max":17.643},"gpu_busy_ms_per_present":5.026,"worker_busy_pct":{"queue worker 0x20":0.935,"queue worker 0x21":0.831,"queue worker 0x0":85.718},"top_phases_ms_per_frame":{"Driver.Packet":14.175,"Driver.Packet.draw":12.722,"Driver.Draw":12.539,"Driver.Draw.draw_enqueued":8.822,"Vulkan.DrawFromRecipe":7.757,"Graphics.DrawWithRecipe":7.696,"Graphics.DrawWithRecipe.draw_and_resource_release":7.674,"Graphics.Draw":4.146},"trace_dropped":false,"trace":"aps5-trace-3fdd1f52e968-121052.json.gz"}
{"commit":"db42fda731b2","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.417},"presents":20,"frame_ms":{"p50":16.684,"p90":17.080,"p99":17.213,"max":17.213},"gpu_busy_ms_per_present":6.057,"worker_busy_pct":{"queue worker 0x20":0.858,"queue worker 0x21":0.893,"queue worker 0x0":84.545},"top_phases_ms_per_frame":{"Driver.Packet":14.265,"Driver.Packet.draw":12.742,"Driver.Draw":12.562,"Driver.Draw.draw_enqueued":8.988,"Vulkan.DrawFromRecipe":7.831,"Graphics.DrawWithRecipe":7.772,"Graphics.DrawWithRecipe.draw_and_resource_release":7.751,"Recorder.Sync":4.253},"trace_dropped":false,"trace":"aps5-trace-db42fda731b2-93052.json.gz"}
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
**`python tools/trace_digest.py` — With Change (`db42fda731b2`, `aps5-trace-db42fda731b2-93052.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 17.1 / p99 17.2 / max 17.2 ms, GPU busy 6.1 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit db42fda731b2, dropped: no

```json
{"commit":"db42fda731b2","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":333.417},"presents":20,"frame_ms":{"p50":16.684,"p90":17.08,"p99":17.213,"max":17.213},"gpu_busy_ms_per_present":6.057,"worker_busy_pct":{"queue worker 0x20":0.858,"queue worker 0x21":0.893,"queue worker 0x0":84.545},"top_phases_ms_per_frame":{"Driver.Packet":14.265,"Driver.Packet.draw":12.742,"Driver.Draw":12.562,"Driver.Draw.draw_enqueued":8.988,"Vulkan.DrawFromRecipe":7.831,"Graphics.DrawWithRecipe":7.772,"Graphics.DrawWithRecipe.draw_and_resource_release":7.751,"Recorder.Sync":4.253},"trace_dropped":false,"trace":"aps5-trace-db42fda731b2-93052.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 14: 17.2 ms at 150 ms: Driver.WorkerWait (queue worker 0x20) 17.1, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 17.1, Driver.WorkerWait (queue worker 0x21) 17.1, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 17.1
- present 5: 17.1 ms at 0 ms: Driver.WorkerWait (queue worker 0x20) 15.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 15.7, Driver.WorkerWait (queue worker 0x21) 15.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 15.5
- present 20: 17.0 ms at 250 ms: Driver.WorkerWait (queue worker 0x21) 16.9, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.9, Driver.WorkerWait (queue worker 0x20) 16.9, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.9
- present 18: 16.9 ms at 217 ms: Driver.WorkerWait (queue worker 0x20) 16.7, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.7, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 13: 16.9 ms at 133 ms: Driver.WorkerWait (queue worker 0x20) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.8, Driver.WorkerWait (queue worker 0x21) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.8

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.7 (94 %) | 0.8 | 0.1 | Driver.EndSubmission 0.1, Driver.Packet 0.1 |
| queue worker 0x21 | 0.1 (1 %) | 15.8 (94 %) | 0.8 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x0 | 8.6 (51 %) | 2.1 (13 %) | 6.0 | 21.4 | Driver.Packet 8.6 |
| draw committer | 7.1 (43 %) | 0.1 (0 %) | 9.5 | 10.0 | Vulkan.DrawFromRecipe 6.6, Graphics.Draw 0.4 |
| presenter | 0.8 (5 %) | 1.2 (7 %) | 14.7 | 0.1 | VideoOut.Flip 1.9, Driver.Present 0.1 |
| gpu | 6.0 (36 %) | 0.0 (0 %) | 10.6 | 0.0 | batch 6.0 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 33.6, Driver.WorkerWait.idle_and_completion_poll 33.5, Driver.Packet 8.7, Driver.Packet.draw 8.1, Driver.Draw 8.0, Driver.Draw.draw_enqueued 7.1, Vulkan.DrawFromRecipe 6.6, Graphics.DrawWithRecipe 6.6, Graphics.DrawWithRecipe.draw_and_resource_release 6.6, batch 6.0

**Longest waits**

- Driver.WorkerWait (queue worker 0x21) 17.2 ms at 301 ms, present 23
- Driver.WorkerWait (queue worker 0x20) 17.1 ms at 301 ms, present 23
- Driver.WorkerWait (queue worker 0x20) 16.9 ms at 135 ms, present 13
- Driver.WorkerWait (queue worker 0x20) 16.9 ms at 152 ms, present 14
- Driver.WorkerWait (queue worker 0x21) 16.9 ms at 135 ms, present 13
- Driver.WorkerWait (queue worker 0x21) 16.9 ms at 251 ms, present 20
- Driver.WorkerWait (queue worker 0x20) 16.9 ms at 251 ms, present 20
- Driver.WorkerWait (queue worker 0x21) 16.8 ms at 152 ms, present 14
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 1 ms, present 5
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 68 ms, present 9

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 296: 1.1 ms at 80 ms, present 9: GPU no ranges; recorded by Recorder.Submit (draw committer) 0.0, Recorder.Reap (draw committer) 0.0
- batch 112: 0.9 ms at 28 ms, present 6: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.0, Graphics.Draw.draw_and_resource_release (draw committer) 0.0, Recorder.Submit (draw committer) 0.0, GpuMutex.wait.draw (draw committer) 0.0
- batch 829: 0.9 ms at 227 ms, present 18: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.1, Graphics.Draw.draw_and_resource_release (draw committer) 0.1, Recorder.Submit (draw committer) 0.0
- batch 55: 0.9 ms at 14 ms, present 5: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.0, Graphics.Draw.draw_and_resource_release (draw committer) 0.0, Recorder.Submit (draw committer) 0.0, Vulkan.DrawFromRecipe (draw committer) 0.0
- batch 710: 0.8 ms at 195 ms, present 16: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.4, Graphics.Draw.draw_and_resource_release (draw committer) 0.4, Resources.Complete (draw committer) 0.3, Recorder.Reap (draw committer) 0.0
````
**`python tools/trace_digest.py` — Interval Summary (`3fdd1f52e968` vs `db42fda731b2`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
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
# With Change (db42fda731b2)
**3 intervals**, 0.0 to 3.0 s, 175 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit db42fda731b2

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 60 | 16.7 | 16.9 | 22.7 | 22.7 | 5.9 | 83 % | Driver.Packet 14.2 |
| 2 | 1-2 | 60 | 16.7 | 16.9 | 18.6 | 18.6 | 5.8 | 80 % | Driver.Packet 13.5 |
| 3 | 2-3 | 55 | 18.5 | 20.0 | 20.7 | 20.7 | 5.6 | 89 % | Driver.Packet 16.5 |

```
p99 ms  @#%  0-22.7
gpu ms  @@%  0-5.9
```
````

---

##### 2. Stacked Full Lane B (`arch/stack-b1-plus-b2-clean` `1f016ef8a7d4` vs `arch/stack-b1-b2-b3-clean` `bfd5d6151379`)

When stacked on top of Lane B.1 + B.2 (`arch/stack-b1-plus-b2-clean` `1f016ef8a7d4`), the full Lane B stack (`arch/stack-b1-b2-b3-clean` `bfd5d6151379`) reduces `Driver.Packet` from `14.257` to `13.873` ms/frame (**-2.7%**) and `p50` frame time to `16.159` ms.

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`1f016ef8a7d4` -> `bfd5d6151379`)**:
  - `frame_ms`: p50 `16.617` -> `16.159` ms | p90 `16.942` -> `16.999` ms | p99 `17.657` -> `18.326` ms | max `17.657` -> `18.326` ms
  - `gpu_busy_ms_per_present`: `6.011` -> `6.03` ms (`0.3%`)
  - `Driver.Packet` (ms/frame): `14.257` -> `13.873` ms (`-2.7%`)
- **Attached Traces**:
  - Baseline (`1f016ef8a7d4`): `U:/scratch/anyps5/traces/stack-b1-plus-b2-clean/aps5-trace-1f016ef8a7d4-62628.json.gz` (summary: `U:/scratch/anyps5/traces/stack-b1-plus-b2-clean/aps5-summary.jsonl`)
  - Change (`bfd5d6151379`): `U:/scratch/anyps5/traces/stack-b1-b2-b3-clean/aps5-trace-bfd5d6151379-93936.json.gz` (summary: `U:/scratch/anyps5/traces/stack-b1-b2-b3-clean/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`1f016ef8a7d4` vs `bfd5d6151379`)**:
```json
{"commit":"1f016ef8a7d4","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":328.138},"presents":20,"frame_ms":{"p50":16.617,"p90":16.942,"p99":17.657,"max":17.657},"gpu_busy_ms_per_present":6.011,"worker_busy_pct":{"queue worker 0x20":0.901,"queue worker 0x21":0.941,"queue worker 0x0":85.819},"top_phases_ms_per_frame":{"Driver.Packet":14.257,"Driver.Packet.draw":12.699,"Driver.Draw":12.516,"Driver.Draw.draw_enqueued":8.793,"Vulkan.DrawFromRecipe":7.686,"Graphics.DrawWithRecipe":7.625,"Graphics.DrawWithRecipe.draw_and_resource_release":7.603,"Graphics.Draw":4.284},"trace_dropped":false,"trace":"aps5-trace-1f016ef8a7d4-62628.json.gz"}
{"commit":"bfd5d6151379","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":318.664},"presents":20,"frame_ms":{"p50":16.159,"p90":16.999,"p99":18.326,"max":18.326},"gpu_busy_ms_per_present":6.030,"worker_busy_pct":{"queue worker 0x20":0.739,"queue worker 0x21":0.838,"queue worker 0x0":86.177},"top_phases_ms_per_frame":{"Driver.Packet":13.873,"Driver.Packet.draw":12.372,"Driver.Draw":12.180,"Driver.Draw.draw_enqueued":8.484,"Vulkan.DrawFromRecipe":7.375,"Graphics.DrawWithRecipe":7.318,"Graphics.DrawWithRecipe.draw_and_resource_release":7.297,"Graphics.Draw":4.290},"trace_dropped":false,"trace":"aps5-trace-bfd5d6151379-93936.json.gz"}
```
**`python tools/trace_digest.py` — Baseline (`1f016ef8a7d4`, `aps5-trace-1f016ef8a7d4-62628.json.gz`)**:
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
**`python tools/trace_digest.py` — With Change (`bfd5d6151379`, `aps5-trace-bfd5d6151379-93936.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.2 / p90 17.0 / p99 18.3 / max 18.3 ms, GPU busy 6.0 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit bfd5d6151379, dropped: no

```json
{"commit":"bfd5d6151379","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":318.664},"presents":20,"frame_ms":{"p50":16.159,"p90":16.999,"p99":18.326,"max":18.326},"gpu_busy_ms_per_present":6.03,"worker_busy_pct":{"queue worker 0x20":0.739,"queue worker 0x21":0.838,"queue worker 0x0":86.177},"top_phases_ms_per_frame":{"Driver.Packet":13.873,"Driver.Packet.draw":12.372,"Driver.Draw":12.18,"Driver.Draw.draw_enqueued":8.484,"Vulkan.DrawFromRecipe":7.375,"Graphics.DrawWithRecipe":7.318,"Graphics.DrawWithRecipe.draw_and_resource_release":7.297,"Graphics.Draw":4.29},"trace_dropped":false,"trace":"aps5-trace-bfd5d6151379-93936.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 11: 18.3 ms at 93 ms: Driver.WorkerWait (queue worker 0x20) 18.2, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 18.2, Driver.WorkerWait (queue worker 0x21) 18.2, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 18.2
- present 20: 17.0 ms at 235 ms: Driver.WorkerWait (queue worker 0x20) 16.9, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.9, Driver.WorkerWait (queue worker 0x21) 16.9, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.9
- present 24: 16.9 ms at 302 ms: Driver.Packet (queue worker 0x0) 8.7, Driver.Packet.draw (queue worker 0x0) 8.1, Driver.Draw (queue worker 0x0) 8.0, Driver.Draw.draw_enqueued (queue worker 0x0) 6.2
- present 12: 16.7 ms at 111 ms: Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 21: 16.6 ms at 252 ms: Driver.WorkerWait (queue worker 0x20) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.5, Driver.WorkerWait (queue worker 0x21) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.5

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.0 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.0 |
| queue worker 0x21 | 0.1 (0 %) | 15.0 (94 %) | 0.9 | 0.1 | Driver.EndSubmission 0.1, Driver.Packet 0.0 |
| queue worker 0x0 | 8.3 (52 %) | 1.7 (11 %) | 5.9 | 21.3 | Driver.Packet 8.3 |
| draw committer | 6.7 (42 %) | 0.0 (0 %) | 9.2 | 10.1 | Vulkan.DrawFromRecipe 6.2, Graphics.Draw 0.4 |
| presenter | 0.8 (5 %) | 0.8 (5 %) | 14.3 | 0.1 | VideoOut.Flip 1.5, Driver.Present 0.0 |
| gpu | 6.1 (38 %) | 0.0 (0 %) | 9.9 | 0.0 | batch 6.1 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 31.7, Driver.WorkerWait.idle_and_completion_poll 31.6, Driver.Packet 8.4, Driver.Packet.draw 7.8, Driver.Draw 7.6, Driver.Draw.draw_enqueued 6.6, Vulkan.DrawFromRecipe 6.2, Graphics.DrawWithRecipe 6.1, Graphics.DrawWithRecipe.draw_and_resource_release 6.1, batch 6.1

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 18.7 ms at 94 ms, present 11
- Driver.WorkerWait (queue worker 0x21) 18.6 ms at 94 ms, present 11
- Driver.WorkerWait (queue worker 0x20) 17.0 ms at 236 ms, present 20
- Driver.WorkerWait (queue worker 0x21) 17.0 ms at 236 ms, present 20
- Driver.WorkerWait (queue worker 0x21) 16.5 ms at 286 ms, present 23
- Driver.WorkerWait (queue worker 0x21) 16.5 ms at 253 ms, present 21
- Driver.WorkerWait (queue worker 0x20) 16.5 ms at 220 ms, present 19
- Driver.WorkerWait (queue worker 0x20) 16.5 ms at 286 ms, present 23
- Driver.WorkerWait (queue worker 0x20) 16.5 ms at 253 ms, present 21
- Driver.WorkerWait (queue worker 0x21) 16.5 ms at 220 ms, present 19

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 1005: 1.0 ms at 263 ms, present 21: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.1, Graphics.Draw.draw_and_resource_release (draw committer) 0.1, GpuMutex.wait.draw (draw committer) 0.0, Recorder.Submit (draw committer) 0.0
- batch 831: 1.0 ms at 216 ms, present 18: GPU no ranges; recorded by Recorder.Submit (draw committer) 0.0
- batch 1064: 0.9 ms at 279 ms, present 22: GPU no ranges; recorded by no CPU slices
- batch 173: 0.9 ms at 42 ms, present 7: GPU no ranges; recorded by no CPU slices
- batch 352: 0.9 ms at 88 ms, present 10: GPU no ranges; recorded by no CPU slices
````
**`python tools/trace_digest.py` — Interval Summary (`1f016ef8a7d4` vs `bfd5d6151379`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
````markdown
# Baseline (1f016ef8a7d4)
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
# With Change (bfd5d6151379)
**3 intervals**, 0.0 to 3.0 s, 174 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit bfd5d6151379

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 60 | 16.7 | 17.8 | 20.9 | 20.9 | 6.1 | 86 % | Driver.Packet 14.6 |
| 2 | 1-2 | 60 | 16.7 | 17.0 | 26.9 | 26.9 | 6.0 | 82 % | Driver.Packet 13.9 |
| 3 | 2-3 | 54 | 18.5 | 20.8 | 22.8 | 22.8 | 5.5 | 88 % | Driver.Packet 16.6 |

```
p99 ms  #@%  0-26.9
gpu ms  @@%  0-6.1
```
````

### Checklist

- [x] `python tools/check_conventions.py` passes
- [x] `python tools/check_test_registration.py build` passes
- [x] New tests fail without the fix and pass with it

Depends on: none
Fixes: none
Unblocks: PPSA01556
GPUs tested: NVIDIA GeForce RTX 5090 (driver 610.52, Windows 11)
AI-assisted: yes
