# NeuralGuard

**v1.5.4** — A personal, habit-learning firewall for Windows 11 — built the
buildable way. Now with an on-device machine-learning tier that scores your
connections in the background (shadow by default, never enforces on its own).

**[⬇ Download the installer](https://github.com/harrisb415/NeuralGuard/releases/latest)**
— `NeuralGuard-Setup-*.exe`, no build tools required. Prefer to build it
yourself? See [`docs/INSTALL.md`](docs/INSTALL.md).

NeuralGuard watches how *you* use the network, learns your normal, and quietly
blocks the stuff that doesn't fit. It is designed to be built and run by one
person on their own machine, not by a team shipping a signed kernel product.

> **Status:** Phases 0–4 done — recording, habit learning, enforcement with
> block-notify-retry prompts, a background Windows service, the on-device ML
> tier (shadow by default), a WinUI 3 dashboard (`gui/`), and an in-app
> updater are all working end to end. Enforcement covers **both directions
> and both IP versions**, with direction read from the WFP layer rather than
> guessed. Inbound enforcement is **opt-in** (`ngd inbound`). v1.5.0 collapsed
> the frontend to **one app**: the service is the only thing that touches the
> firewall, and it remembers the mode you left it in. What's left is the
> *optional* Phase 5 kernel callout driver — see
> [`docs/ROADMAP.md`](docs/ROADMAP.md), [`docs/DECISIONS.md`](docs/DECISIONS.md)
> for the coverage model, and [`CHANGELOG.md`](CHANGELOG.md) for each release.

---

## The one-paragraph pitch

Windows already has a good packet path: the **Windows Filtering Platform (WFP)**,
the kernel engine that Windows Defender Firewall itself is just one consumer of.
NeuralGuard becomes another WFP consumer with its own sublayer, so its decisions
win regardless of Defender's rules — and for the first several phases it does this
entirely from **user mode**, with no kernel driver to sign, crash, or lock you out.
It spends its first weeks in **learning mode** (enforcing nothing, recording your
baseline), then flips to **enforcement**: your learned habits are auto-permitted,
anything genuinely new gets blocked-and-prompted, and a lightweight on-device model
reviews *completed* flows to decide what should be trusted next time. The AI writes
rules; deterministic code enforces them.

You drive it from **one app** (`NeuralGuard.exe`): a tray icon showing the current
mode, prompts to allow or block new connections, and a dashboard window (live feed,
recent blocks, rule management, stats) with a panic button — backed by a headless
`ngctl` for scripting. It never does firewall work itself; it tells the `ngd`
service what to do over a named pipe. Two processes is the floor, not a choice: the
service runs as `LocalSystem` in session 0 and **cannot** show UI, so something in
your session has to own the icon and answer the prompts.

## Why this exists (and what it replaces)

This project is the pragmatic redesign of an earlier, far more ambitious spec —
"NeuralGuard: On-Device ML Enforcement Engine," a production/enterprise document
written for a 3-person team, a WHQL-signed kernel callout driver, EV certificates,
enterprise MDM/GPO, ARM64 NPU inference, and federated learning. That document is a
great north-star vision, but it commits to the single hardest path in the whole space
as its *foundation*, and its core ML premise has a train/serve flaw.

NeuralGuard-as-built keeps the good bones (WFP sublayer, tiered decisions, ring
buffer, ONNX habit model, SQLite policy store) and fixes four things. See
[`docs/DECISIONS.md`](docs/DECISIONS.md) for the full before/after.

| # | Flaw in the original spec | Fix here |
|---|---------------------------|----------|
| 1 | Model must decide at the SYN, but is trained on *completed-flow* features that don't exist yet (train/serve skew) | **Two decisions.** First contact is decided by cheap deterministic + novelty signals; ML runs on *completed* flows and governs the *next* connection. |
| 2 | Verdict cache keyed on the full 5-tuple, whose ephemeral source port makes it (nearly) never hit | **Identity key** = `(process signer/hash, destination domain-or-ASN, port, protocol)` — stable across connections. |
| 3 | Throughput targets in Gbps, but filtering happens per-connection (ALE), not per-byte | **Measure the right things**: new-flow verdict latency, prompt rate, false-block rate, habit cache-hit rate. Not Gbps. |
| 4 | "Hold the SYN, ask the AI" specified without the pend APIs it requires; and gated on a signed kernel driver | **No driver in v1.** Default-deny + block-notify-retry (the simplewall/TinyWall pattern). A callout driver with true `FwpsPendClassify0` is an *optional* later phase. |

## What it is not

No cloud. No telemetry leaves the machine. No enterprise MDM/GPO, no Windows
Security Center registration (that's a gated Microsoft partner program), no EV
cert or WHQL for v1, no federated learning, no NPU requirement, no 10 GbE claims.
Those were dropped on purpose — see [`docs/DECISIONS.md`](docs/DECISIONS.md).

## Documents

- [`docs/INSTALL.md`](docs/INSTALL.md) — build it and get it running.
- [`docs/DESIGN.md`](docs/DESIGN.md) — architecture, the tiered decision model, identity, habit engine, safety rails.
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — the solo, phase-by-phase build plan.
- [`docs/DECISIONS.md`](docs/DECISIONS.md) — what changed from the original NeuralGuard spec and why.
- [`CHANGELOG.md`](CHANGELOG.md) — what shipped in each release.

## Planned stack

C++20 · [WIL](https://github.com/microsoft/wil) for handle/error hygiene · WFP
management API (`fwpuclnt`) · SQLite · ONNX Runtime (Phase 4) · CMake + vcpkg.
Reference codebases worth reading first: [`simplewall`](https://github.com/henrypp/simplewall)
and Microsoft's `WFPSampler`.

## Safety, up front

Enforcement-mode firewalls lock you out of your own machine the first time they
have a bug — including RDP and, if you're unlucky, the path to ship the fix.
Non-negotiables baked into the design:

- Develop against a **Hyper-V VM**, never the physical dev box, until enforcement is trusted.
- A **panic switch** — one click in the tray menu, or `ngctl panic` — that deletes NeuralGuard's WFP sublayer filters and fails open.
- **Always-exempt** loopback, DHCP, DNS, and NTP from default-deny.
- v1 **fails open** on agent crash (a personal machine offline is worse than a missed block).

## License

MIT — see [`LICENSE`](LICENSE).
