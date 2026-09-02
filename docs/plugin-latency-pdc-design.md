# Plugin Latency, PDC, and Process Load

Status: accepted — confirmed 2026-09-02

## Goal

RoudaMix will expose runtime Plugin Latency, provide sample-exact Plugin Delay Compensation for full-PDC outputs, preserve a user-controlled low-latency monitoring path, and report per-instance Plugin Process Load. Device Latency remains a separate measurement and is never folded into Total Plugin Delay.

## Output latency policies

- The system Monitor Output defaults to `lowLatency`.
- The system Stream Output defaults to `fullPdc`.
- User-created Output Tracks can select either policy and default to `fullPdc`.
- A `fullPdc` output aligns incoming branches at every DAG summing point. Muted paths remain in the plan so mute/unmute does not change timing.
- A `lowLatency` output adds no Compensation Delay and does not promise that parallel input paths are synchronized.
- All `lowLatency` outputs share the same per-slot Monitor Bypass state. Monitor Bypass may be configured before any low-latency route exists.
- Global bypass affects every output. Monitor Bypass affects only low-latency outputs.

## Latency model

- Each active primary or shadow instance uses its own `getLatencySamples()` result.
- A bypassed plugin contributes zero effective latency but retains its declared value for diagnostics.
- A placeholder contributes zero effective latency and displays unknown latency.
- Chain Latency is the sum of effective Plugin Latency in chain order.
- Path Latency is accumulated along a routed source-to-output path.
- Total Plugin Delay is the maximum effective Path Latency reaching a given output.
- The header displays Monitor and Stream totals separately as `plug M x.x / S y.y ms`.
- While running, an output with no effective plugin path displays `0.0 ms`. While stopped, totals display `—`; the drawer may retain last-known values only when marked non-live.
- Header values use one decimal place. Drawer latency values use three decimal places. No UI latency value is displayed in samples, although samples remain authoritative in the engine and protocol.

## Full-PDC planning and safety

- Full PDC inserts integer-sample delay at every summing point, not only at the final output.
- Steady-state alignment must be sample-exact. Muted paths remain planned.
- PDC buffers are allocated and graph plans are built on the control thread. The audio callback performs no allocation, lock acquisition, plugin loading, or system call.
- A path may require at most two seconds of delay, and all PDC buffers together may consume at most 256 MiB.
- A user mutation that would exceed either limit fails transactionally and leaves plugin state, UI state, and the current audio graph unchanged.
- Runtime latency changes are applied at audio-block boundaries. Old and new delay reads use a linear 20 ms crossfade.
- A plugin process failure outputs dry audio delayed by that plugin's declared latency, preserving the configured path position for that block.

## Dynamic latency and transactions

- Latency is queried after initialization and relevant structural operations, and again after `restartComponent(kLatencyChanged)`.
- RoudaMix does not poll plugins that violate the VST3 notification contract.
- Preset loads, global bypass, Monitor Bypass, sample-rate changes, shadow creation, and other latency-affecting changes save the old state first. Plugin state, shadows, resources, and the new PDC plan must all succeed before commit.
- A runtime primary latency that exceeds limits suspends the primary and sends dry audio through all outputs. A shadow-only violation degrades only low-latency outputs; Stream continues using the primary.
- Runtime suspension and degraded shadow state persist visibly and emit one notice. A valid change or explicit retry triggers state synchronization, pre-roll, transactional graph rebuild, and automatic recovery.
- Runtime observations (`kLatencyChanged`, Process Load, degradation, suspension, and recovery) do not dirty the Session. User intent changes do.

## Monitor shadow instances

- Primary instances are the only user-editable state authority.
- When Monitor and Stream processing diverge, independent shadow instances are created on the control thread. One state applies to every low-latency output.
- Shadow initialization copies full component/controller state in memory, then reapplies host-authoritative parameter values.
- Later parameter, preset, and bypass changes are mirrored from primary to shadow. Shadows do not expose an independent editor.
- A newly required shadow silently pre-rolls for 500 ms, then enters the graph through the 20 ms crossfade. UI shows the monitor path as preparing during pre-roll.
- Shadows are released when no low-latency route needs the divergent chain.
- Shadow load or synchronization failure sends dry audio through the affected low-latency branch and marks it degraded; primary Stream processing continues.
- Shadows are runtime-derived and are not stored as separate Session instances.

## Session v3

- Saving this feature writes `roudamixSession: 3`.
- Each plugin slot persists `monitorBypassed: bool`.
- Each Output Track persists `latencyPolicy: "fullPdc" | "lowLatency"`.
- Loading v2 assigns `lowLatency` to the system Monitor Output, `fullPdc` to all other outputs, and `monitorBypassed: false` to every plugin.
- Runtime latency values, Process Load, shadow instances, degraded state, and runtime suspension are never persisted.

## Protocol and refresh behavior

- Engine snapshots advertise the `pluginLatencyPdcV1` capability.
- UI displays latency and PDC features only when that capability is present.
- Ordinary `status` events carry Monitor/Stream totals, `latencyGeneration`, and each slot's runtime latency/state fields.
- The non-modal drawer calls `get_latency_report` on open and whenever an incoming status changes `latencyGeneration`.
- The complete report describes output totals, path totals, Compensation Delay, Track/chain membership, instance variants, states, and affected outputs.
- The full report is not repeated in ordinary status events. Process Load remains in SHM telemetry.
- Transactional UI controls remain pending until the engine confirms success. Failure preserves the old control value and creates a notice.

## Process Load telemetry

- The user-facing metric is named `Process Load`, not system CPU.
- For each primary or shadow process instance, load is the serialized and overhead-adjusted TSC cycles spent in plugin processing divided by wall-clock TSC cycles over the publish window. It represents a fraction of one logical CPU and is directly comparable with callback load.
- Telemetry publishes at 30 Hz through a dedicated 256-entry table in the next telemetry ABI. Existing 64-entry audio meter strips remain unchanged.
- Primary instances receive entries first; remaining entries are assigned to shadows in graph order. Overflow displays unavailable.
- UI shows a one-second EWMA and a rolling five-second peak, each with one decimal place.
- Bypassed, placeholder, runtime-suspended, stopped, unmetered, and ABI-incompatible instances display `—`, not `0.0%` or stale values.
- Primary and Monitor Shadow processing are separate rows.
- A telemetry ABI mismatch disables only Process Load and emits one notice; status-based latency and PDC remain available.
- Plugin-specific notices are not emitted solely from a high load number. When callback overload occurs, one consolidated notice is emitted and the drawer highlights the largest contributors.
- With 128 zero-work primary/shadow process calls, instrumentation may increase callback load by no more than one percentage point.

## User interface

- Clicking the header `plug M/S` control opens a non-modal right-side drawer.
- The drawer remains usable while the mixer is edited and continuously receives Process Load updates.
- The drawer first shows one summary per Output Track, including policy, Total Plugin Delay, Compensation Delay, and synchronization status.
- It then shows a single Track/chain instance hierarchy. Instances are not duplicated per output; each row lists affected outputs.
- Rows expose primary/shadow role, Plugin Latency, Process Load, five-second peak, runtime state, and path context. Columns are sortable while preserving Output/Track grouping.
- Low-Latency Outputs display a compact state marker and `data-tooltip` explaining that parallel paths are not guaranteed to be synchronized. No persistent warning banner is used.
- Output Tracks expose their latency policy in the strip.
- Plugin rows expose Monitor Bypass on hover/focus and show a persistent marker while enabled, pending, or degraded. The same action is also available in the existing right-click menu.
- Every new tooltip uses `data-tooltip`; pure-icon controls additionally provide an accurate `aria-label`. No native HTML `title` is introduced.

## Verification

- A deterministic test-build VST3 fixture can select latency, emit `kLatencyChanged`, consume controlled CPU time, and fail processing.
- Pure planner tests and engine integration tests cover 44.1, 48, 96, and 192 kHz; block sizes 64, 128, 512, and 2048; latency values 0, 1, block-minus-one, block, block-plus-one, multi-block, and the two-second boundary.
- Graph tests cover serial chains, fan-in, fan-out, nested buses, mute, global bypass, Monitor Bypass, shadow branches, runtime suspension, recovery, and transaction rollback.
- Stable full-PDC output is verified with impulses and must be sample-exact outside transition windows.
- A transition must contain no NaN, infinity, or unexpected silent gap, must complete the linear 20 ms crossfade, and must return to sample-exact output afterward.
- Session tests cover v2-to-v3 migration and v3 round trips without persisting runtime-derived state.
- Telemetry tests cover ABI mismatch, 256-entry overflow, primary-first allocation, smoothing, peak expiry, and the instrumentation overhead budget.
- UI tests cover pending/error rollback, drawer generation refresh, stopped/stale states, sorting/grouping, accessibility names, and `tooltip-policy.test.ts`.

## Rollout

Implementation is split internally across engine planning, VST3 hosting, protocol/session migration, telemetry, and UI/testing. The capability remains disabled and all UI remains hidden until the complete verification suite passes; there is no public display-only intermediate release that could be mistaken for completed PDC.

## Decision records

- [Split latency policy by output role](./adr/0001-split-latency-policy-by-output-role.md)
- [Use shadow instances for monitor branches](./adr/0002-use-shadow-instances-for-monitor-branches.md)
- [Separate plugin load from meter telemetry](./adr/0003-separate-plugin-load-from-meter-telemetry.md)
- [Migrate latency policy in Session v3](./adr/0004-migrate-latency-policy-in-session-v3.md)
- [Gate latency features by engine capability](./adr/0005-gate-latency-features-by-engine-capability.md)
- [Fetch latency reports on demand](./adr/0006-fetch-latency-reports-on-demand.md)
