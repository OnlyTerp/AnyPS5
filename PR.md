### What

All guest queues previously submitted into a single `VkQueue`, so a CPU read waiting for one queue's batch waited for every batch any queue had submitted before it, and switching between guest queues serialized independent GPU work onto one timeline.

- Create up to `MaxRecorderStreams` (16) Vulkan queues of the graphics queue family when timeline semaphores are enabled (gated by `APS5_NO_SEPARATE_QUEUES`), and give each recorder stream its own `VkQueue`, open `Batch`, timeline semaphore, and monotonic value counter.
- Assign each guest queue tag its own recorder stream on first recording (queue 0 and untagged threads use stream 0; additional guest queues wrap across streams `1 .. N-1`), and synchronize `vkQueueSubmit` with `settleQueueMutex` when a recorder stream shares the settle queue handle.
- Order two recorder streams on the GPU via timeline semaphore waits (`OrderAfterLabel`, `OrderAfterValue`, `OrderAfterQueue`, and `DeclareSection`, gated by `APS5_NO_QUEUE_ORDER`) only where a `WAIT_REG_MEM` / `WAIT_REG_MEM_64` packet, arrival order after queue 0, or a shared emulator object (resource set, workgroup memory buffer, storage image, attachment, target view, proxy, sampled texture image, or fill pattern buffer) requires it.
- Walk `labels.lower_bound(address & ~std::uint64_t{3})` once per range in `OrderAfterLabel` and `OrderAfterValue` instead of probing every dword individually, and expose `Recorder::LabelTableLookups()`.
- Track multi-stream completion epochs (`CallerToken`, `AllToken`, `Reached`, `WaitToken`, `OtherQueuePoints`) and retire signaled batches in dependency order across streams.

### Tested

- `agc_driver_stream_order_tests`:
  - Green (default on NVIDIA GeForce RTX 5090): `stream order tests passed (16 recorder streams)`
  - Red (`APS5_NO_QUEUE_ORDER=1`): fails deterministically (`AGC graphics: stream order: word 0 of the fill that reuses another queue's pattern buffer holds 0x00000000, not 0x11111111 (16 recorder streams)`).
- `agc_driver_queue_label_order_tests`:
  - Green (default on NVIDIA GeForce RTX 5090): `queue label order tests passed (16 recorder streams)`
  - Red (`APS5_NO_QUEUE_ORDER=1`): fails deterministically (`AGC graphics: queue label order: 11 cases failed` across compare functions 1–6, 64-bit masks, waits before/after writer, and arrival order).


#### Performance report

##### 1. PR onto `main` (`origin/main` `3fdd1f52e968` vs `arch/d-per-guest-vulkan-queues-clean` `c37a11d0f968`)

Assigning each guest queue (`0x0`, `0x20`, `0x21`) its own `VkQueue` recorder stream and timeline semaphore with `lower_bound` label ordering improves `p50` (`16.677` -> `15.992` ms), `p90` (`17.288` -> `17.166` ms), and `p99` (`17.643` -> `17.331` ms) frame times.

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`3fdd1f52e968` -> `c37a11d0f968`)**:
  - `frame_ms`: p50 `16.677` -> `15.992` ms | p90 `17.288` -> `17.166` ms | p99 `17.643` -> `17.331` ms | max `17.643` -> `17.331` ms
  - `gpu_busy_ms_per_present`: `5.026` -> `5.333` ms (`6.1%`)
  - `Driver.Packet` (ms/frame): `14.175` -> `14.162` ms (`-0.1%`)
- **Attached Traces**:
  - Baseline (`3fdd1f52e968`): `U:/scratch/anyps5/traces/origin-main/aps5-trace-3fdd1f52e968-121052.json.gz` (summary: `U:/scratch/anyps5/traces/origin-main/aps5-summary.jsonl`)
  - Change (`c37a11d0f968`): `U:/scratch/anyps5/traces/d-per-guest-vulkan-queues-clean/aps5-trace-c37a11d0f968-112720.json.gz` (summary: `U:/scratch/anyps5/traces/d-per-guest-vulkan-queues-clean/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`3fdd1f52e968` vs `c37a11d0f968`)**:
```json
{"commit":"3fdd1f52e968","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":326.986},"presents":20,"frame_ms":{"p50":16.677,"p90":17.288,"p99":17.643,"max":17.643},"gpu_busy_ms_per_present":5.026,"worker_busy_pct":{"queue worker 0x20":0.935,"queue worker 0x21":0.831,"queue worker 0x0":85.718},"top_phases_ms_per_frame":{"Driver.Packet":14.175,"Driver.Packet.draw":12.722,"Driver.Draw":12.539,"Driver.Draw.draw_enqueued":8.822,"Vulkan.DrawFromRecipe":7.757,"Graphics.DrawWithRecipe":7.696,"Graphics.DrawWithRecipe.draw_and_resource_release":7.674,"Graphics.Draw":4.146},"trace_dropped":false,"trace":"aps5-trace-3fdd1f52e968-121052.json.gz"}
{"commit":"c37a11d0f968","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":319.618},"presents":20,"frame_ms":{"p50":15.992,"p90":17.166,"p99":17.331,"max":17.331},"gpu_busy_ms_per_present":5.333,"worker_busy_pct":{"queue worker 0x20":0.840,"queue worker 0x21":0.922,"queue worker 0x0":87.579},"top_phases_ms_per_frame":{"Driver.Packet":14.162,"Driver.Packet.draw":12.735,"Driver.Draw":12.539,"Driver.Draw.draw_enqueued":9.061,"Vulkan.DrawFromRecipe":7.904,"Graphics.DrawWithRecipe":7.838,"Graphics.DrawWithRecipe.draw_and_resource_release":7.817,"Graphics.Draw":4.157},"trace_dropped":false,"trace":"aps5-trace-c37a11d0f968-112720.json.gz"}
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
**`python tools/trace_digest.py` — With Change (`c37a11d0f968`, `aps5-trace-c37a11d0f968-112720.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.0 / p90 17.2 / p99 17.3 / max 17.3 ms, GPU busy 5.3 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit c37a11d0f968, dropped: no

```json
{"commit":"c37a11d0f968","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":319.618},"presents":20,"frame_ms":{"p50":15.992,"p90":17.166,"p99":17.331,"max":17.331},"gpu_busy_ms_per_present":5.333,"worker_busy_pct":{"queue worker 0x20":0.84,"queue worker 0x21":0.922,"queue worker 0x0":87.579},"top_phases_ms_per_frame":{"Driver.Packet":14.162,"Driver.Packet.draw":12.735,"Driver.Draw":12.539,"Driver.Draw.draw_enqueued":9.061,"Vulkan.DrawFromRecipe":7.904,"Graphics.DrawWithRecipe":7.838,"Graphics.DrawWithRecipe.draw_and_resource_release":7.817,"Graphics.Draw":4.157},"trace_dropped":false,"trace":"aps5-trace-c37a11d0f968-112720.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 11: 17.3 ms at 96 ms: Driver.WorkerWait (queue worker 0x20) 17.2, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 17.2, Driver.WorkerWait (queue worker 0x21) 17.2, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 17.2
- present 13: 17.2 ms at 128 ms: Driver.WorkerWait (queue worker 0x20) 17.0, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 17.0, Driver.WorkerWait (queue worker 0x21) 17.0, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 17.0
- present 5: 16.9 ms at 0 ms: Driver.WorkerWait (queue worker 0x20) 15.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 15.5, Driver.WorkerWait (queue worker 0x21) 15.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 15.5
- present 7: 16.8 ms at 33 ms: Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 21: 16.7 ms at 253 ms: Driver.WorkerWait (queue worker 0x20) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.5, Driver.WorkerWait (queue worker 0x21) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.5

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.0 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.0 |
| queue worker 0x21 | 0.1 (0 %) | 15.0 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x0 | 8.3 (52 %) | 1.5 (10 %) | 6.2 | 22.2 | Driver.Packet 8.3 |
| draw committer | 6.8 (43 %) | 0.0 (0 %) | 9.1 | 10.7 | Vulkan.DrawFromRecipe 6.5, Graphics.Draw 0.3 |
| presenter | 0.7 (4 %) | 0.7 (4 %) | 14.6 | 0.1 | VideoOut.Flip 1.2, Driver.Present 0.0 |
| gpu | 5.3 (33 %) | 0.0 (0 %) | 10.7 | 0.0 | batch 5.3 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 31.5, Driver.WorkerWait.idle_and_completion_poll 31.5, Driver.Packet 8.4, Driver.Packet.draw 7.9, Driver.Draw 7.7, Driver.Draw.draw_enqueued 6.8, Vulkan.DrawFromRecipe 6.5, Graphics.DrawWithRecipe 6.4, Graphics.DrawWithRecipe.draw_and_resource_release 6.4, batch 5.3

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 17.2 ms at 97 ms, present 11
- Driver.WorkerWait (queue worker 0x21) 17.2 ms at 97 ms, present 11
- Driver.WorkerWait (queue worker 0x20) 17.0 ms at 130 ms, present 13
- Driver.WorkerWait (queue worker 0x21) 16.9 ms at 130 ms, present 13
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 1 ms, present 5
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 34 ms, present 7
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 34 ms, present 7
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 1 ms, present 5
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 254 ms, present 21
- Driver.WorkerWait (queue worker 0x20) 16.5 ms at 288 ms, present 23

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 715: 0.7 ms at 189 ms, present 16: GPU no ranges; recorded by no CPU slices
- batch 477: 0.7 ms at 126 ms, present 12: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.0, Graphics.Draw.draw_and_resource_release (draw committer) 0.0, Recorder.Reap (draw committer) 0.0, Recorder.Submit (draw committer) 0.0
- batch 892: 0.6 ms at 231 ms, present 19: GPU no ranges; recorded by Vulkan.DrawFromRecipe (draw committer) 0.1, Graphics.DrawWithRecipe (draw committer) 0.1, Graphics.DrawWithRecipe.draw_and_resource_release (draw committer) 0.1, Graphics.Draw (draw committer) 0.0
- batch 411: 0.6 ms at 106 ms, present 11: GPU no ranges; recorded by Vulkan.DrawFromRecipe (draw committer) 0.1, Graphics.DrawWithRecipe (draw committer) 0.1, Graphics.DrawWithRecipe.draw_and_resource_release (draw committer) 0.1, Graphics.Draw (draw committer) 0.0
- batch 1020: 0.6 ms at 266 ms, present 21: GPU no ranges; recorded by Graphics.Draw (draw committer) 0.0, Graphics.Draw.draw_and_resource_release (draw committer) 0.0, Recorder.Submit (draw committer) 0.0, Resources.Prepare (draw committer) 0.0
````
**`python tools/trace_digest.py` — Interval Summary (`3fdd1f52e968` vs `c37a11d0f968`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
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
# With Change (c37a11d0f968)
**3 intervals**, 0.0 to 3.0 s, 174 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit c37a11d0f968

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.6 | 16.8 | 20.2 | 20.2 | 5.2 | 83 % | Driver.Packet 13.9 |
| 2 | 1-2 | 59 | 16.7 | 19.2 | 20.8 | 20.8 | 5.0 | 82 % | Driver.Packet 14.4 |
| 3 | 2-3 | 54 | 18.6 | 20.1 | 21.4 | 21.4 | 5.0 | 91 % | Driver.Packet 17.2 |

```
p99 ms  %@@  0-21.4
gpu ms  @@@  0-5.2
```
````

---

##### 2. Stacked Lane D (`arch/d-unlock-pipeline-and-present-clean` `3c9517994bfa` vs `arch/stack-d1-plus-d2-clean` `d30cb7b008e6`)

When stacked on top of Lane D.1 (`arch/d-unlock-pipeline-and-present-clean` `3c9517994bfa`), the combined D.1 + D.2 stack (`arch/stack-d1-plus-d2-clean` `d30cb7b008e6`) further reduces `gpu_busy_ms_per_present` (`5.198` -> `5.155` ms) and improves `p99` (`17.331` -> `17.006` ms) and `Driver.Packet` (`14.162` -> `14.051` ms) relative to standalone D.2.

- **Scene & Machine**: Unified Village + Multi-Queue AGC Scene (`aps5_scene_bench.exe 16 850`, `1920x1080`, `APS5_TRACE_FRAMES=5:20` windowed capture & `APS5_TRACE_SUMMARY_EVERY=1` 3s interval capture) on `NVIDIA GeForce RTX 5090` (`NVIDIA 610.52`, `Windows 10.0.26200`, `dirty: false`).
- **Metric Comparison (`3c9517994bfa` -> `d30cb7b008e6`)**:
  - `frame_ms`: p50 `16.677` -> `16.661` ms | p90 `16.787` -> `16.936` ms | p99 `16.876` -> `17.006` ms | max `16.876` -> `17.006` ms
  - `gpu_busy_ms_per_present`: `5.198` -> `5.155` ms (`-0.8%`)
  - `Driver.Packet` (ms/frame): `13.288` -> `14.051` ms (`5.7%`)
- **Attached Traces**:
  - Baseline (`3c9517994bfa`): `U:/scratch/anyps5/traces/d-unlock-pipeline-and-present-clean/aps5-trace-3c9517994bfa-65236.json.gz` (summary: `U:/scratch/anyps5/traces/d-unlock-pipeline-and-present-clean/aps5-summary.jsonl`)
  - Change (`d30cb7b008e6`): `U:/scratch/anyps5/traces/stack-d1-plus-d2-clean/aps5-trace-d30cb7b008e6-12272.json.gz` (summary: `U:/scratch/anyps5/traces/stack-d1-plus-d2-clean/aps5-summary.jsonl`)
**Paired `aps5-summary.jsonl` lines (`3c9517994bfa` vs `d30cb7b008e6`)**:
```json
{"commit":"3c9517994bfa","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":324.449},"presents":20,"frame_ms":{"p50":16.677,"p90":16.787,"p99":16.876,"max":16.876},"gpu_busy_ms_per_present":5.198,"worker_busy_pct":{"queue worker 0x20":1.022,"queue worker 0x21":1.281,"queue worker 0x0":80.536},"top_phases_ms_per_frame":{"Driver.Packet":13.288,"Driver.Packet.draw":11.722,"Driver.Draw":11.546,"Driver.Draw.draw_enqueued":8.160,"Vulkan.DrawFromRecipe":7.291,"Graphics.DrawWithRecipe":7.232,"Graphics.DrawWithRecipe.draw_and_resource_release":7.211,"Recorder.Sync":3.943},"trace_dropped":false,"trace":"aps5-trace-3c9517994bfa-65236.json.gz"}
{"commit":"d30cb7b008e6","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":328.131},"presents":20,"frame_ms":{"p50":16.661,"p90":16.936,"p99":17.006,"max":17.006},"gpu_busy_ms_per_present":5.155,"worker_busy_pct":{"queue worker 0x20":1.127,"queue worker 0x21":1.320,"queue worker 0x0":84.188},"top_phases_ms_per_frame":{"Driver.Packet":14.051,"Driver.Packet.draw":12.551,"Driver.Draw":12.344,"Driver.Draw.draw_enqueued":8.820,"Vulkan.DrawFromRecipe":7.607,"Graphics.DrawWithRecipe":7.540,"Graphics.DrawWithRecipe.draw_and_resource_release":7.520,"Graphics.Draw":4.169},"trace_dropped":false,"trace":"aps5-trace-d30cb7b008e6-12272.json.gz"}
```
**`python tools/trace_digest.py` — Baseline (`3c9517994bfa`, `aps5-trace-3c9517994bfa-65236.json.gz`)**:
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
**`python tools/trace_digest.py` — With Change (`d30cb7b008e6`, `aps5-trace-d30cb7b008e6-12272.json.gz`)**:
````markdown
**frames 5:20** from present 5: 20 presents, frame p50 16.7 / p90 16.9 / p99 17.0 / max 17.0 ms, GPU busy 5.2 ms per present
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit d30cb7b008e6, dropped: no

```json
{"commit":"d30cb7b008e6","dirty":false,"env":{"APS5_TRACE_DIR":"<path>","APS5_TRACE_FRAMES":"5:20"},"gpu":"NVIDIA GeForce RTX 5090","driver":"NVIDIA 610.52","os":"Windows 10.0.26200","resolution":"1920x1080","window":{"spec":"frames 5:20","first_present":5,"ms":328.131},"presents":20,"frame_ms":{"p50":16.661,"p90":16.936,"p99":17.006,"max":17.006},"gpu_busy_ms_per_present":5.155,"worker_busy_pct":{"queue worker 0x20":1.127,"queue worker 0x21":1.32,"queue worker 0x0":84.188},"top_phases_ms_per_frame":{"Driver.Packet":14.051,"Driver.Packet.draw":12.551,"Driver.Draw":12.344,"Driver.Draw.draw_enqueued":8.82,"Vulkan.DrawFromRecipe":7.607,"Graphics.DrawWithRecipe":7.54,"Graphics.DrawWithRecipe.draw_and_resource_release":7.52,"Graphics.Draw":4.169},"trace_dropped":false,"trace":"aps5-trace-d30cb7b008e6-12272.json.gz"}
```

**Slowest frames** (largest slices inside each, inclusive)

- present 17: 17.0 ms at 195 ms: Driver.WorkerWait (queue worker 0x20) 16.5, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.5, Driver.WorkerWait (queue worker 0x21) 16.4, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.4
- present 19: 16.9 ms at 228 ms: Driver.WorkerWait (queue worker 0x20) 16.8, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.8, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 15: 16.8 ms at 161 ms: Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 13: 16.8 ms at 128 ms: Driver.WorkerWait (queue worker 0x20) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x20) 16.6, Driver.WorkerWait (queue worker 0x21) 16.6, Driver.WorkerWait.idle_and_completion_poll (queue worker 0x21) 16.6
- present 24: 16.7 ms at 311 ms: Driver.Packet (queue worker 0x0) 7.6, Driver.Packet.draw (queue worker 0x0) 7.2, Driver.Draw (queue worker 0x0) 7.1, Driver.Draw.draw_enqueued (queue worker 0x0) 6.4

**Threads** (ms per frame)

| thread | busy | wait | untraced | in slices < 20 us | largest |
|---|---|---|---|---|---|
| queue worker 0x20 | 0.1 (1 %) | 15.4 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x21 | 0.1 (1 %) | 15.5 (94 %) | 0.9 | 0.1 | Driver.Packet 0.1, Driver.EndSubmission 0.1 |
| queue worker 0x0 | 8.0 (49 %) | 2.1 (13 %) | 6.2 | 22.1 | Driver.Packet 8.0 |
| draw committer | 6.5 (39 %) | 0.1 (0 %) | 9.9 | 10.9 | Vulkan.DrawFromRecipe 6.2, Graphics.Draw 0.2 |
| presenter | 0.7 (4 %) | 1.4 (8 %) | 14.3 | 0.1 | VideoOut.Flip 1.9, Driver.Present 0.0 |
| gpu | 5.2 (31 %) | 0.0 (0 %) | 11.3 | 0.0 | batch 5.2 |

**Largest slices** (ms per frame, inclusive, all threads): Driver.WorkerWait 32.9, Driver.WorkerWait.idle_and_completion_poll 32.9, Driver.Packet 8.2, Driver.Packet.draw 7.6, Driver.Draw 7.5, Driver.Draw.draw_enqueued 6.6, Vulkan.DrawFromRecipe 6.2, Graphics.DrawWithRecipe 6.1, Graphics.DrawWithRecipe.draw_and_resource_release 6.1, batch 5.2

**Longest waits**

- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 229 ms, present 19
- Driver.WorkerWait (queue worker 0x20) 16.8 ms at 129 ms, present 13
- Driver.WorkerWait (queue worker 0x21) 16.8 ms at 129 ms, present 13
- Driver.WorkerWait (queue worker 0x21) 16.7 ms at 229 ms, present 19
- Driver.WorkerWait (queue worker 0x20) 16.6 ms at 296 ms, present 23
- Driver.WorkerWait (queue worker 0x21) 16.6 ms at 263 ms, present 21
- Driver.WorkerWait (queue worker 0x21) 16.5 ms at 96 ms, present 11
- Driver.WorkerWait (queue worker 0x20) 16.5 ms at 96 ms, present 11
- Driver.WorkerWait (queue worker 0x20) 16.5 ms at 263 ms, present 21
- Driver.WorkerWait (queue worker 0x20) 16.5 ms at 163 ms, present 15

**Largest GPU batches** (GPU ranges inside, and the CPU slices that recorded each, inclusive)

- batch 1090: 0.7 ms at 291 ms, present 22: GPU no ranges; recorded by Recorder.Submit (draw committer) 0.0, GpuMutex.wait.draw (draw committer) 0.0
- batch 540: 0.7 ms at 141 ms, present 13: GPU no ranges; recorded by no CPU slices
- batch 718: 0.6 ms at 187 ms, present 16: GPU no ranges; recorded by Vulkan.DrawFromRecipe (draw committer) 0.1, Graphics.DrawWithRecipe (draw committer) 0.1, Graphics.DrawWithRecipe.draw_and_resource_release (draw committer) 0.1, Graphics.Draw (draw committer) 0.0
- batch 661: 0.6 ms at 173 ms, present 15: GPU no ranges; recorded by no CPU slices
- batch 234: 0.6 ms at 56 ms, present 8: GPU no ranges; recorded by Vulkan.DrawFromRecipe (draw committer) 0.1, Graphics.DrawWithRecipe (draw committer) 0.1, Graphics.DrawWithRecipe.draw_and_resource_release (draw committer) 0.1, Graphics.Draw (draw committer) 0.0
````
**`python tools/trace_digest.py` — Interval Summary (`3c9517994bfa` vs `d30cb7b008e6`, `APS5_TRACE_SUMMARY_EVERY=1`)**:
````markdown
# Baseline (3c9517994bfa)
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
# With Change (d30cb7b008e6)
**3 intervals**, 0.0 to 3.0 s, 172 presents
NVIDIA GeForce RTX 5090, NVIDIA 610.52, Windows 10.0.26200, 1920x1080, commit d30cb7b008e6

| # | seconds | presents | p50 | p90 | p99 | max | GPU busy | busiest worker | largest phase |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0-1 | 61 | 16.7 | 16.8 | 19.6 | 19.6 | 5.1 | 82 % | Driver.Packet 13.8 |
| 2 | 1-2 | 59 | 16.7 | 18.9 | 21.9 | 21.9 | 4.9 | 83 % | Driver.Packet 14.4 |
| 3 | 2-3 | 52 | 19.3 | 21.7 | 23.1 | 23.1 | 5.0 | 91 % | Driver.Packet 17.9 |

```
p99 ms  %@@  0-23.1
gpu ms  @@@  0-5.1
```
````

### Checklist

- [x] Compiler warnings resolved
- [x] Conventions check passed (`tools/check_conventions.py`)
- [x] Formatting check passed
- [x] Registered and ran automated tests (`tools/check_test_registration.py`)
- [x] Updated documentation (`docs/dev/ENVIRONMENT.md`)
- [x] Depends on: none
- [x] Fixes: none
- [x] Unblocks: none
- [x] GPUs tested: NVIDIA GeForce RTX 5090 (Windows 11)
- [x] AI-assisted: yes
