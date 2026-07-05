# NeuralGuard — Roadmap

Solo build plan. Phases are ordered so that **each one produces something usable on
its own** and de-risks the next. Durations are effort-shaped, not calendar
deadlines; several phases are gated by "let it run and collect data," which is wall-
clock time, not work.

The guiding rule: **you have a working, safe tool at the end of Phase 2.**
Everything after that makes it smarter, not functional-for-the-first-time.

---

## Phase 0 — WFP telemetry spike  ✅ DONE

**Goal:** prove the whole foundation in ~200 lines. No enforcement, no ML, no DB.

- Open a WFP engine session (`FwpmEngineOpen0`), subscribe to net events
  (`FwpmNetEventSubscribe4`), and print each allow/drop with the 5-tuple, image
  path (from `appId`), and user SID. Implemented in [`src/ngmon/main.cpp`](../src/ngmon/main.cpp).

**Result:** working — `ngmon` streams live connections attributed to processes on
the test VM (built on host via VS 2026, run on the VM over SSH).

**Two gotchas found and documented in the code:**
1. Events need engine-side enablement — `FwpmEngineSetOption0(FWPM_ENGINE_COLLECT_NET_EVENTS)`
   plus the `CLASSIFY_ALLOW` keyword — or you get *zero* events, not just missing allows.
2. Those options can't be set from a `FWPM_SESSION_FLAG_DYNAMIC` session
   (`FWP_E_DYNAMIC_SESSION_IN_PROGRESS`), so `ngmon` uses a normal session.
   Also enable the *Filtering Platform Connection* audit subcategory
   (`auditpol /set /subcategory:"Filtering Platform Connection" /success:enable /failure:enable`).

## Phase 1 — Learning mode (passive)

**Goal:** record a real baseline. Still enforces nothing.

- Stand up `ngd` as a proper Windows service and `ngctl status`.
- Add the SQLite store (`ngpolicy.db`) and schema for flow events + habit tables.
- Build the live `PID → identity` table and the DNS-ETW correlation (IP → domain).
- Compute and persist the **identity key** (§4 of DESIGN) per connection; accumulate
  habit counts with decay.
- `ngctl log` / `ngctl rules` to inspect what's being learned.

**Progress:**
- ✅ Recorder — `ngd record` persists every WFP net event to `ngpolicy.db`
  (`flow_events`); `ngd dump` reads them back. SQLite vendored (amalgamation,
  no package manager); WFP helpers shared with `ngmon` via `src/common/wfp_util.h`.
- ✅ Process identity — `\device\harddiskvolumeN\...` normalized to `C:\...`,
  image SHA-256, and Authenticode signer (embedded **and catalog**, so in-box
  Windows binaries resolve to their Microsoft signer). De-duplicated in a
  `process_identity` table and cached in memory.
- ✅ Refactor — split the growing `ngd/main.cpp` into an `ngcore` static lib
  (`db`, `identity`, `signer`, `util`) + `ngd` (`recorder`, `main`), so later
  binaries (ngctl, service, tray backend) reuse the core.
- ⬜ DNS correlation — consume `Microsoft-Windows-DNS-Client` ETW, map IP → domain.
- ⬜ Identity key + decaying habit counts (see DESIGN §4–5).

**Gate to next phase:** run on the physical machine for a few weeks of normal use.
It's safe — it's read-only. Stop when a normal day is ~95% covered by learned keys
(watch the "novel connections per day" curve flatten).

## Phase 2 — Enforcement + prompts  ← *the day it replaces your built-in firewall*

**Goal:** actually block, without locking yourself out.

- Create NeuralGuard's own **sublayer** (weight above Defender's range).
- **Build the panic switch first** (`ngctl panic` → delete all filters in our
  sublayer). Do not add a single blocking filter before this exists.
- Hard-code the always-exempt Tier 0 rules (loopback/DHCP/DNS/NTP).
- Auto-generate permit filters (`FwpmFilterAdd0`) for the learned baseline; install a
  default-block for the rest.
- Ship a **minimal `ngtray`**: tray icon showing mode (Learning / Enforcing / Panic),
  **actionable toast prompts** (Allow once / Always / Block) for blocked novel
  connections, and **Panic** in the right-click menu. `ngctl` stays as the headless
  fallback.
- Implement block-notify-retry behind those prompts: *Allow* writes a permit rule and
  the app's retry succeeds.
- Watchdog + fail-open-on-death.

**Gate to next phase:** a week of daily use on the physical box with near-zero false
blocks and a manageable prompt rate.

## Phase 3 — Habit scoring & autonomy

**Goal:** fewer prompts, smarter defaults — still no ML.

- Nightly compaction: decay, evict, and **promote** stable keys to explicit rules.
- Novelty score (recency + time-of-day + destination fan-out) drives auto-allow for
  low-novelty connections so you stop being asked about obvious ones.
- Configurable autonomy: `prompt-everything` → `auto-allow-low-novelty` →
  `auto-allow-and-summarize`.
- Expand `ngtray` from prompts-only into a full **dashboard window**: live connection
  feed, recent blocks with reasons, rule management (add / edit / remove), habit
  stats, the autonomy toggle, and the weekly digest rendered in-app.
- Weekly text digest of what changed (no model yet — just the frequency stats).

## Phase 4 — ML on completed flows

**Goal:** the "AI" earns its name, correctly — scoring finished flows, not SYNs.

- Archive completed-flow feature vectors (opt-in, auto-purged) for training data.
- Off-device training pipeline: completed-flow features + a public IDS dataset for
  malicious classes → LightGBM → ONNX (INT8).
- In `ngd`: ONNX Runtime CPU session scores completed flows asynchronously; outputs
  become **block-next-time proposals** and **anomaly flags** feeding the Phase-3
  promotion job behind a confidence gate.
- Optional: an offline LLM turns the weekly digest into prose and highlights the few
  things worth a human look. Advisory only — it never enforces.

## Phase 5 — (Optional) kernel callout driver

**Goal:** only if Phases 0–4 prove it's worth it.

- A KMDF WFP **callout driver** for the things user mode genuinely can't do: true
  **pend-and-prompt** at the SYN (`FwpsPendClassify0` → `FwpsCompleteClassify0` →
  `FwpsReleaseClassifyHandle0`), and per-packet features (timing, entropy) for
  inspected flows via the SPSC ring buffer.
- This is where EV cert + WHQL attestation, HVCI compatibility, Driver Verifier, and
  VM-only development become mandatory. It is a project in itself — hence last, and
  optional.

---

## Deliberately out of scope (see DECISIONS.md)

Windows Security Center / Action Center registration (gated Microsoft partner
program), enterprise MDM/GPO/ADMX, WFAS `.wfw` import/export, federated learning +
differential privacy, cloud model CDN + canary infra, ARM64/QNN NPU inference, EV
cert & WHQL for v1, and any Gbps throughput target. None of these serve a personal,
single-machine tool; each is re-openable if the project ever grows up into a product.

## Stack & references

C++20 · WIL · WFP management API (`fwpuclnt.lib`) · SQLite · ONNX Runtime (Phase 4)
· CMake + vcpkg. Read `simplewall` (henrypp) and Microsoft's `WFPSampler` before
writing Phase 0.
