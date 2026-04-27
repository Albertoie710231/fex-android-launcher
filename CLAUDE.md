# Project: FEX-Emu DXVK port (leegao Vulkan wrapper target)

## Hard rules — these are standing constraints, not turn-local instructions

1. Before proposing ANY change to allocator, descriptor pool, synchronization, command buffer, or resource-lifetime code, you MUST do these three things in order, in the response, before writing any code or running any tool:
     a. Name the memory files relevant to the subsystem you are touching.
     b. Quote the specific lines from those files that constrain the design.
     c. State what those constraints forbid for the change you are about to make.
   If you skip this, the change is wrong by default. If memory is silent on the subsystem, say so explicitly — do not infer.

2. Do not write "Direct answer:", "The fix is:", or any confident architectural claim before completing step 1. Confidence without memory consultation is the failure mode this file exists to prevent.

3. "We already tried this" is the default assumption, not the exception. Memory contains prior attempts AND why they failed. If your proposed solution sounds clean and obvious, it has probably already been tried and the failure documented. Search before proposing.

## Project-specific priors (do not design around these without verification)

- The leegao Vulkan wrapper does NOT honor spec semantics. Assume nothing about: `vkResetDescriptorPool` reference dropping, synchronization primitive per-spec behavior, descriptor set lifetime, or memory reclamation timing. Design against documented leegao behavior in memory, not Vulkan spec text.
- DXVK upstream patches that depend on conformant pool reset, conformant sync, or driver-side memory reclamation will misbehave here. Backports must be evaluated against leegao behavior before adoption.
- `vm.max_map_count` exhaustion is a recurring failure mode for FEX in this configuration. Memory may pin the workaround.
- The Mali wrapper's exposed Vulkan extension list is authoritatively dumped in `reference_mali_wrapper_extensions_2026_04_24.md`. Don't guess what's exposed; read.

## Memory protocol

- Memory files live in `/home/alberto/.claude/projects/-home-alberto-Documentos-fex-android-launcher/memory/`. Filename format: `project_<topic>_<YYYY_MM_DD>.md` for time-bound state, `reference_<topic>.md` for durable facts, `feedback_<topic>.md` for user-given guidance.
- `MEMORY.md` is the index — always loaded — but content lives in the linked files. Open them.
- Use `grep -rni` across the directory at the start of every design decision, not only at session start. Re-consult when the topic shifts mid-session.
- Topic shift = you are now reasoning about a subsystem you were not reasoning about three turns ago. Re-grep.
- After `/compact`, re-read this file and re-grep for the active topic before resuming work. Compaction discards prior memory consultation.
- When writing a `project_*_plan_*.md` memory entry that proposes a design, include a **"Constraints already in memory"** section that explicitly cites every memory entry whose content bears on the design's viability — even (especially) if the plan's premise might contradict them. The act of citing forces the cross-check.

## Stop conditions — ask the user, do not guess

- Memory entries on the topic contradict each other.
- Memory contradicts the Vulkan spec and you cannot tell which governs.
- The design depends on wrapper behavior not yet documented in memory.
- A proposed fix resembles a previously-failed attempt and you cannot articulate why this attempt is meaningfully different.

## Dependencies — never add without explicit authorization

The user does not want extra dependencies in this repo or any submodule. "Try X" or "do X" is **not** authorization to add dependencies that X happens to require. Authorization to add a dependency is a separate, explicit ask.

This includes (non-exhaustive):

- Adding a git remote (e.g. `git remote add` to a third-party fork).
- Cloning third-party repos into the workspace.
- Downloading external code, patches, or binaries from the internet.
- Installing system packages, Python packages, npm packages.
- Adding a meson/cmake subproject or `subprojects/` entry.
- Submodule `git submodule add`.
- Bundling additional binaries into APK assets.

If the task you are asked to do **cannot proceed without** adding such a dependency, **stop and ask before doing it**, with this format:

> "Task X requires Y (one-sentence reason). Y means [adding a remote / cloning / downloading / installing] [specific URL / package]. OK to proceed?"

The bar is: the user must say yes specifically to that dependency, not implicitly via authorizing the parent task. "Try option 2" did NOT authorize adding `https://github.com/Sporif/dxvk-async.git` as a remote; that was the failure mode of 2026-04-25 PM.

Counter-examples that are NOT new dependencies and don't need this gate:

- Building from source already in the repo.
- Modifying files already tracked.
- Reading public docs / spec text and implementing locally.
- Pulling already-tracked git history (`git fetch origin`).

## Style

- Direct technical prose. No hedging preamble. No "great question."
- When reverting, state what is reverted and why in one line, then revert.
- Don't use Monitor tools that emit on every poll cycle. Monitors are for events that warrant interrupting (crash signatures, threshold breaches, completions). For silent watching, use a watchdog with strict event filters; otherwise let the user read tracer output themselves.

## Operational rules

### Validating perf changes

The whole point of any DXVK / descriptor / view / memory optimization on this stack is the **stress path** (Sekiro stress-area gameplay, Ys IX battle scenes — see project memory for which game has which bad zone). Title screens, menus, intro logos exercise small, stable working sets — they exercise the optimization's best case but tell you nothing about the worst case.

A "47× improvement" measured at the title screen is suspect by default. Don't write a memory entry claiming success until the validation has driven into the documented stress area for ≥2 minutes and reached a steady-state plateau (or crashed). If you can't reach the stress area, say so explicitly. Don't infer steady-state behavior from menu metrics.

### Probes

When the user says "set the probes" they mean **all** of:

- `scripts/trace_mem_v2.sh <interval>` — host-side, polls `/proc/<sekiro>/status` via `adb shell` → `/sdcard/mem_v2.csv`. Tracks RSS / MemAvail / smaps_rollup over time. **This is the probe that proves whether the change crashed the OS.** It is the difference between "metrics looked good" and "we know the run was actually healthy."
- `scripts/trace_smaps.sh <interval> <max_samples>` — host-side, pulls full `/proc/<sekiro>/smaps` to host `/tmp/smaps/`.
- The Vulkan implicit layers (`MALI_ALLOC_PROBE=1`, `WRAPPER_POOL_RECYCLER=1`, `VK_GC_ENABLE=1`) — auto-loaded by `NativeWinePipeline` env vars at game launch.

Run the trace scripts via `Bash run_in_background=true`, not shell `&`. Shell-backgrounded jobs lose context across Bash tool invocations.

Don't substitute one probe for another. RSS-on-tablet (mem_v2) and per-submit alloc rate (VK_GC) measure different things. A Vulkan-layer "47× win" while mem_v2 quietly climbs to OOM is the disaster pattern of 2026-04-25.

### When the user gives a narrow operational instruction, do exactly that

Pattern that broke this session: user said "set the probes" → I launched Sekiro myself, set up monitors, started a watchdog, asked clarifying questions, emitted periodic status updates that flooded chat. Each step was unrequested.

- Do only what was asked.
- Do not also launch the game, set up monitors, install unrelated layers, or send periodic progress.
- Suggest follow-ups in one sentence after completing the request — don't act on them.
- If the user repeats the instruction, re-read memory and check for the script/file/state they're pointing at. Don't insist on what you've already done.

### Project-specific operational gotchas

- **The Sekiro process's `comm` is `sekiro.exe`** (10 chars), but `pidof sekiro.exe` may return empty in some adb shell contexts. Use `ps -A | grep sekiro` or read `/proc/[0-9]*/comm` via `run-as` for reliable detection.
- **`/data/data/com.mediatek.steamlauncher/files/` is unreadable from `adb shell`** (different SELinux domain). Use `adb shell run-as com.mediatek.steamlauncher cat ...` for reads.
- **`adb shell run-as ... grep "a|b"` interprets `|` as a shell pipe and breaks.** Pull files to host first, then grep on host.
- **The wine stderr live log is `files/ys9_stderr.live.log`**, not `sekiro_stderr.log`. The non-live log only writes when `wineRun` returns; for a still-running game, only the .live.log is current.
- **The DXVK fork at `~/Documentos/Proton-Arm64/dxvk/` is a git submodule** with uncommitted patches (pool-sizing). When restoring or comparing baselines, `git diff` shows what's on top of HEAD; the "previously working state" is HEAD + those uncommitted edits, not HEAD alone. `git checkout HEAD -- <file>` will lose the patches.
- **`./build_arm64_pe_dlls.sh dxvk` requires Docker** — if the daemon is down, ask the user to `sudo systemctl start docker` rather than trying to start it yourself.
- **Always grep `NativeWinePipeline.kt` for what env vars it sets** before assuming a layer is or isn't loaded. The probes are wired there; reading the source is faster than guessing.

### When a fix isn't working, question the premise before band-aiding

When stress-zone metrics in the cache run jumped to 500 set_allocs/submit (worse than baseline 234), my first instinct was to add a `kMaxCachedSets` cap to evict more aggressively. That's a band-aid: it patches the symptom of "cache is growing unbounded" without asking "is the design valid on this stack at all?" The design wasn't.

Rule: when a fix shows degraded behavior under stress, before adding logic to compensate, re-read the memory you used to design it. If memory says the underlying primitive doesn't behave the way you assumed, the fix is wrong; revert.

### No autonomous commits

Per `feedback_ask_before_every_commit.md` — only commit when the user explicitly asks AND something works end-to-end (not just "compiles"). Auto mode does not override this. Memory entries can be added freely; submodule patches and parent-repo commits cannot.
