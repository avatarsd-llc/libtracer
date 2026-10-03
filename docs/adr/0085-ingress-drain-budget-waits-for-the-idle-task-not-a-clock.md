# A link's ingress drain is bounded by a compile-time budget, and a spent budget waits for the core's idle task, never for a clock

<!-- status: accepted -->

Status: **accepted** (2026-10-03, maintainer ruling; ships in v0.18.0). Applies [ADR-0081](0081-pre-sink-ingress-native-window-hold-or-named-drop-never-parked.md) §2 (hold ingress in the transport's native window) to a link that is already delivering, and keeps the standing no-timers rule ([`CLAUDE.md`](../../CLAUDE.md) §Design rules). The knob follows the compile-time-by-default rule of [ADR-0068](0068-build-configuration-is-plain-cpp-config-header.md).

## Context

`httpd_ws_link_t` reads its peers on the `esp_http_server` task (priority 5). The server's loop `select()`s on every session socket and calls the link's URI handler once per readable request, and the handler reads one WebSocket frame. While a peer keeps sending, the socket is readable again as soon as the handler returns, so `select()` never blocks. Together with the TCP/IP and Wi-Fi tasks that bring the bytes in, the httpd task keeps the core busy for as long as the peer keeps sending.

On a single-core ESP32 target the idle task then never runs. The task watchdog watches the idle task, so a peer that sends without pause for longer than the watchdog period resets the board. On an ESP32-C6 a burst of about 2,000 small writes over 15–16 s starved the idle task for 10–14 s (httpd 41 %, TCP/IP 30 %, Wi-Fi 19 % of the CPU) and the 15 s watchdog fired. Faster Wi-Fi only shortened the burst.

Two fixes were measured on silicon. Yielding for 2 ticks once the idle task had been starved for 100 ms did not help: the TCP/IP and Wi-Fi tasks and the priority-2 tasks filled each yield. Pausing the ingress for about 20 ms worked (the longest starvation fell to 0.89 s) because 20 ms is long enough for the peer's TCP window to close. But a pause measured in milliseconds is a timer and a clock read, which libtracer does not allow.

## Decision

**1. A drain has a budget of frames and bytes.** A **drain** is the run of frames a link's receive context consumes while its core never goes idle. When the drain has consumed `kRxDrainFrames` frames or `kRxDrainBytes` payload bytes, the link stops reading before the next frame's payload. Either budget ends the drain, and the frame that crosses the byte line is read whole.

**2. A spent budget waits for the core's idle task.** The link blocks the receive context on a semaphore that the core's idle hook gives. The idle task runs only when no other task on that core is ready, so the wait ends exactly when the core has had one idle moment, however long the higher-priority work takes. This is the event the watchdog is waiting for, and it needs no timer and no clock read. The idle hook is installed once per core per process, through `esp_register_freertos_idle_hook_for_cpu`. Each idle pass advances a per-core epoch with one store, and gives the semaphore only while a link is waiting.

**3. Back-pressure is TCP's.** While the receive context waits, the frame's payload and everything behind it stay in the socket. The lwIP receive window fills, the peer's TCP stack stops sending, and the TCP/IP and Wi-Fi tasks go quiet. That quiet is what lets the idle task run. No frame is dropped and no buffer is added to the library ([ADR-0081](0081-pre-sink-ingress-native-window-hold-or-named-drop-never-parked.md) §1–§2).

**4. A core that idles on its own resets the drain.** Each frame reads the core's idle epoch. If the epoch has moved since the previous frame, a new drain starts at zero. A link whose peers do not saturate the core never waits, so the budget costs nothing on a node that is not flooded.

**5. The budget is a compile-time trait.** `tr::graph::default_config_t::kRxDrainFrames` (default 32) and `kRxDrainBytes` (default 32,768) are part of the build's one `config_t`, with `tr::net` spellings. The ESP-IDF component binds them from `CONFIG_LIBTRACER_WS_SERVER_RX_DRAIN_FRAMES` and `CONFIG_LIBTRACER_WS_SERVER_RX_DRAIN_BYTES`. `0` turns a budget off; with both off the gate compiles to nothing and no hook is installed. The ESP-IDF defaults are 32 and 32,768 only when the task watchdog watches the idle task of every core httpd may run on, and `0` otherwise (see Consequences: a non-zero budget requires a core that idles). The receiver pays: the cost lands on the node being flooded, and only while it is.

**6. The wait is counted.** `httpd_ws_link_t::stats().rx_drain_waits` counts the times a drain ended on its budget. It is not a drop counter: a rising count means the TCP window is pacing a peer that sends faster than the node can serve.

## Why these defaults

The bound this fix gives is: the idle task is kept out for at most one budget of frames plus the higher-priority work that follows it. At the ~7.3 ms of board time per small request measured on the ESP32-C6, 32 frames is about 0.23 s (32 × 7.3 ms = 0.234 s). That is under a quarter of the shortest task watchdog IDF allows (1 s) and about a twentieth of its default (5 s), so the default is safe for any watchdog setting without being tuned per board.

The cost of a wait is the time until the core idles. When the flood is the only load, that is the time the TCP/IP and Wi-Fi tasks need to settle once the window has closed. That cost is paid once per 32 frames rather than once per frame, and the silicon run will measure it (below). A smaller budget tightens the bound and pays the wait more often. A larger one does the opposite, and at about 130 frames it would use half the 1 s minimum watchdog.

The byte default is the link's own frame cap (`kMaxFrameBytes`, 32 KiB), so one maximum-size frame is one drain. It is also about six default lwIP receive windows (5,760 B). Frames bound a flood of small writes, where the per-request overhead dominates. Bytes bound a flood of large frames, where the copy and decode dominate.

## Consequences

- **No timer, no clock.** The fix adds an idle hook, one static binary semaphore per core, and four small members per link (the drain epoch, its frame and byte counts, and the wait counter). It adds no thread and reads no time.
- **The whole httpd task waits, not just one session.** Other sessions on the same server, and the link's queued sends (which run as httpd control work), are delayed by the same wait. The wait lasts only until the core idles, which is short once the flood is held back by TCP.
- **A spent budget waits for every lower-priority task on the core (priority inversion).** The idle task runs only once every other task on its core has blocked. So after one budget of back-to-back frames, the httpd task (priority 5) yields not just to the TCP/IP and Wi-Fi tasks but to every lower-priority ready task on its core, until all of them block. Before this fix httpd preempted them. A priority-2 task in the middle of 500 ms of work now holds the next frame back for up to those 500 ms. Under sustained load, the link's read rate is therefore bounded by the longest run of lower-priority work on its core. That is inherent in "wait for idle" and acceptable for a watchdog fix, but it is the latency an embedder will notice: it applies only once a budget is spent, and never to a link that leaves its core idle now and then.
- **A non-zero budget requires a core that idles.** If a task below httpd's priority never blocks on the httpd task's core, the idle task never runs. A drained link then waits for good: every session on the server stops, queued sends stop, and `httpd_stop()` and the link's destructor block behind the parked handler. With the task watchdog watching that idle task, the board was already being reset, so the budget changes nothing. But IDF lets an application turn that check off (`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n`) precisely because it runs such a task, and there a link that worked before would stop after 32 frames over the life of the process. So the ESP-IDF options default to `0` (no pacing) unless the task watchdog watches the idle task of every core httpd may run on (`ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0`, plus `_CPU1` on a dual-core build). The component's CMake warns when an explicit non-zero budget is combined with the CPU0 check off. A build that turned the check off must leave both budgets at 0. The protection lives in the build configuration, so the host test cannot express it; the host test does show that a parked link stays parked until the idle hooks run.
- **A full hook table leaves that core unpaced.** IDF holds eight idle hooks per core. If registration fails, the link logs it once ("ingress unpaced") and does not pace on that core, because waiting for a hook that never runs would hang the receive context (`httpd_ws_drain_budget_unhooked` covers this). `stats().rx_drain_waits` then reads 0, which is indistinguishable from a link that never saturated its core. The log line is the only signal.
- **Dual-core targets: the budget is approximate for an unpinned httpd task.** The gate is per core, and a waiting link waits for the idle task of the core it is running on. `HTTPD_DEFAULT_CONFIG` leaves the httpd task unpinned, so it may migrate between frames. A drain records the core it started on. When the task finds itself on another core, it starts a new drain there rather than compare epochs across cores. A migrating task can therefore read more than one budget between idle moments of any single core. It never hangs, because it always waits on the gate of the core it is on. On dual-core targets the watchdog pressure is far lower, and pinning the httpd task (`httpd_config_t::core_id`) makes the budget exact.
- **Host links are unchanged.** Hosted transports read from their own threads under a preemptive OS and do not read the trait.

## On-silicon validation

The host test (`httpd_ws_drain_budget_test`) shows the counting: a sustained ingress parks after exactly one budget, holds the next payload unread, and resumes only when the idle hooks run. The silicon run must show the rest. On an ESP32-C6 (single core) with the default budget and the advisory's reproducer (bursts of about 2,000 small writes over 15–16 s, task watchdog at 5 s and at 15 s):

1. **The watchdog no longer fires.** No task-watchdog reset over at least 10 consecutive bursts.
2. **The longest idle-task starvation per burst.** Measure it with the same instrument as the original report. Expect well under 1 s, compared with 10.3–13.9 s before the fix.
3. **Throughput cost.** Requests per second over the burst, with the default budget and with both budgets set to 0 (the pre-fix behaviour, run with the watchdog disabled or longer than the burst). Report the ratio. For comparison, the 20 ms pause cost about 21 % (102/s).
4. **`stats().rx_drain_waits`** at the end of a burst, to confirm the budget is what paced it (about burst frames / 32).
5. **Sensitivity.** Repeat 2 and 3 with `CONFIG_LIBTRACER_WS_SERVER_RX_DRAIN_FRAMES` set to 8 and 128 to find where the starvation bound and the throughput cost cross.
6. **No regression on a quiet link.** On an interactive session (the SPA), `rx_drain_waits` stays at 0.

## Alternatives considered

- **A time-bounded pause (about 20 ms).** It worked on silicon, but it is a timer and a clock read. It would also pick the length of the pause in the library, when the right length depends on the board's TCP/IP and Wi-Fi timing.
- **Yield for a few ticks once the idle task has been starved for a while.** Measured on silicon and it failed: higher-priority tasks fill the yield, and detecting the starvation needs a clock.
- **An app-driven pacing seam** (the application tells the link when to resume). It reads no clock, but every embedder would have to drive it, and a node that forgot would either never resume or never pause. The idle task already marks the event the watchdog needs.
- **Lower the httpd task's priority while it drains.** That depends on round-robin time slicing at the idle priority, which is a tick-driven timer in all but name, and it changes the priority of a task the link may not own (the adopted-server constructor).
- **Close or drop a peer that sends too fast.** A rate limit needs a clock, and dropping data is a worse answer than letting TCP hold it when TCP can.
- **Exclude the socket from httpd's `select()` and resume it later.** IDF offers that only through its async-request API, and the resume would have to be posted from the idle hook through the control socket, which can block. The idle hook must not block.
