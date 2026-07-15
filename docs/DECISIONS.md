# NeuralGuard — Design Decisions

Why this repo diverges from the original "NeuralGuard: On-Device ML Enforcement
Engine — Production Technical Specification." That document is a strong vision and
this project keeps its good bones; the decisions below are the corrections and the
scope cuts. ADR-style: each records the original position, the problem, and the call.

---

## The four correctness fixes

### D-1 — ML scores *completed flows*, not SYNs

- **Original:** every new flow is classified at `ALE_AUTH_CONNECT` by a 30-feature
  ONNX model, and blocked in <50 ms if malicious.
- **Problem:** the discriminative features — `bytes_total`, `packet_count`,
  `inter_packet_interval`, `payload_entropy`, `duration`, TLS/HTTP fields — do not
  exist at connect time; the spec itself labels them "per-flow / populated at
  FLOW_CLOSE." The model is then *trained* on CIC-IDS2018, whose records are
  completed bidirectional flow statistics. Training on full-flow features and
  inferring on connect-time features is train/serve skew; the advertised
  ≥97% AUC / <0.5% FP will not survive deployment.
- **Decision:** split into two decisions. The **connect** decision uses only
  features that exist at connect time (identity, port, protocol, time-of-day, geo/
  ASN, recent-frequency aggregates) via deterministic + habit logic. The **model**
  runs asynchronously on **completed** flows and governs *future* connections
  (block-next-time). Train and serve on the same shape → no skew, and the ML earns
  its keep on the problem it can actually solve.

### D-2 — Habit/verdict key is process+destination identity, not the 5-tuple

- **Original:** kernel fast-path verdict cache keyed on the full 5-tuple
  `(src_ip, dst_ip, src_port, dst_port, proto)`.
- **Problem:** the source port is ephemeral — a new value on every connection. A
  5-tuple key therefore misses for every *new* connection, even to a destination you
  reach constantly, so the "<1 µs fast path handles 99%" claim collapses into a
  slow-path ML round-trip per new flow.
- **Decision:** key habits and caches on
  `(process signer-thumbprint-or-image-hash, destination registrable-domain-or-ASN,
  remote_port, protocol)`. Stable across connections; a CDN's many IPs collapse to
  one habit; the fast path actually gets to be fast.

### D-3 — Measure per-connection behaviour, not throughput

- **Original:** success criteria include fast-path throughput ≥1 Gbps (PoC) / ≥10
  Gbps (prod).
- **Problem:** Gbps is a per-byte, data-path metric. Decisions happen per-connection
  at the ALE layer, off the byte path — so the target is either trivially met
  (we're not in the data path) or, once STREAM/DATAGRAM inspection is on, wildly
  optimistic for a user-mode round-trip. Category error.
- **Decision:** track new-connection verdict latency, first-contact prompt rate,
  false-block rate, habit cache-hit rate, and baseline coverage. Throughput becomes
  a real metric only for opted-in inspected flows if a Phase-5 driver ever exists.

### D-4 — No kernel driver in v1; enforce via block-notify-retry

- **Original:** a WFP **callout driver** holds the SYN in the kernel, asks user-mode
  AI, then releases it — and this is the *foundation*, required from Week 1, with EV
  cert + WHQL signing.
- **Problem:** (a) the "hold then release" handshake needs `FwpsPendClassify0` /
  `FwpsCompleteClassify0` / `FwpsReleaseClassifyHandle0`; the spec references only
  the completion call and never the pend/handle plumbing — the exact mechanism the
  design hinges on is under-specified. (b) A signed kernel callout driver is the
  single hardest, riskiest path in the space (BSODs, HVCI, EV cert, WHQL) and making
  it the foundation blocks everything else behind it.
- **Decision:** v1 is entirely **user-mode** (WFP management API). Enforcement is
  default-deny + **block-notify-retry** (the proven `simplewall`/TinyWall pattern):
  block the novel connection, prompt, and on *Allow* add a permit rule so the app's
  retry succeeds. A callout driver with a *correct* pend handshake is an **optional
  Phase 5**, taken only if it proves worth the cost.

---

## The enforcement coverage model

### D-5 — Direction is a WFP *fact* (the layer), never inferred from ports

- **Original (this repo's own early implementation, not the spec):** enforcement
  was outbound-IPv4-only, and the learning path decided whether a connection was
  inbound or outbound by testing `remotePort < 49152` — i.e. guessing direction
  from whether the remote port looked like a "service" port or an ephemeral one.
- **Problem:** it's a guess with real failure modes in both directions. An outbound
  connection to a peer listening on a high port (P2P, some game/QUIC services) is
  wrongly excluded from the baseline; an inbound connection from a client that
  happened to use a low source port is wrongly included. A firewall that mis-attributes
  direction either lets traffic slip by or blocks the wrong thing. The heuristic was
  never a design choice — it was a noise-reduction patch for a bug where inbound
  peers' ephemeral ports created junk one-off habits.
- **Decision:** direction comes from the **WFP layer the event was classified at**,
  which *is* the direction: `ALE_AUTH_CONNECT_V4/V6` = outbound,
  `ALE_AUTH_RECV_ACCEPT_V4/V6` = inbound. Net events carry a `layerId`; we resolve
  the four ALE layer ids once at engine-open and map every event definitively.
  The port heuristic is deleted. Both `habits` and `flow_events` record a
  `direction` column; rows predating it stay NULL and simply don't qualify for a
  baseline rather than being mislabelled.

### D-6 — Enforce at the ALE layers, both directions, both IP versions

- **Decision:** filters are installed at all four ALE connection layers. Outbound
  (`CONNECT_V4` + `CONNECT_V6`) is always on; inbound (`RECV_ACCEPT_V4` + `V6`) is
  **opt-in** via `meta('inbound_mode')` (default `off`), because turning inbound
  default-deny on affects a machine's *listening services* and must never be a
  surprise from an upgrade. Inbound is always *learned* regardless of the mode, and
  `ngd inbound` previews exactly which services would be permitted before you enable it.
- **Why inbound default-deny is safe to offer at all:** `RECV_ACCEPT` fires only on
  **new inbound accepts**. The return traffic of a connection *you* initiated
  outbound is never re-classified there, so inbound enforcement does not touch
  browsing, DNS replies, or any established flow — only new connections *to* your
  listening sockets.
- **Anti-lockout is structural, not incidental:** the inbound Tier-0 permits
  (SSH 22, RDP 3389, DHCP/DHCPv6, loopback, link-local) are installed at weight 15
  *before* the weight-0 catch-all, so no baseline, rule, or ML demotion can ever cut
  off the management channel.

### D-7 — No transport-layer filters: ICMP is already covered at ALE

- **Original plan (ours):** add `OUTBOUND/INBOUND_TRANSPORT_V4/V6` filters, on the
  assumption that ICMP/ICMPv6 "aren't ALE connections" and would therefore be a
  silent hole in a default-deny built only at the ALE layers.
- **Problem:** that assumption is **false**, and we measured it rather than trusting
  it. WFP's ALE layers classify ICMP as a connection-like flow, so the existing
  outbound catch-all already blocks it. Verified on the VM: with outbound
  default-deny active, `ping 8.8.8.8` returns **"General failure"** (the signature of
  a WFP block) at 100% loss; with enforcement off the same ping replies normally. The
  Tier-0 *address* exemptions apply to ICMP correctly too — ICMP to an RFC1918 LAN
  address is permitted (no "General failure"; it simply gets no reply because the
  peer's own firewall drops inbound echo), while public ICMP is blocked.
- **Decision:** **do not add transport-layer filters.** They would be redundant
  outbound, and actively harmful inbound: a blanket inbound ICMP block breaks Path
  MTU Discovery (ICMP fragmentation-needed / ICMPv6 *Packet Too Big* — IPv6 cannot
  fragment, so this blackholes large transfers) and would break IPv6 Neighbor
  Discovery outright. Inbound ICMP is left unfiltered deliberately: its security
  value is low (reconnaissance/nuisance), Windows Firewall already drops inbound echo
  by default, and the breakage risk is high and *subtle*. This is a scope decision,
  not an oversight.
- **Corollary — outbound ICMPv6 still works because of the address exemptions.**
  `fe80::/10` and `ff00::/8` are Tier-0 exempt precisely so Neighbor Discovery /
  Router Discovery / MLD survive the v6 outbound default-deny. Those two exemptions
  are load-bearing: without them, enabling IPv6 enforcement takes IPv6 down.

### D-8 — What "zero coverage gaps" honestly means

- **Decision:** within user-mode WFP, coverage is complete for all
  **connection-attributable** traffic — TCP connects/accepts, UDP flows, and ICMP —
  in both directions and both IP versions, with direction attributed from the layer
  rather than guessed.
- **What is still out of reach, and named rather than papered over:** raw sockets
  crafted below the ALE layers, and true per-packet inspection (timing, entropy).
  Those need the kernel callout driver of **Phase 5** (see D-4). We do not claim
  kernel-level completeness without that driver.

---

## Scope cuts (dropped for a personal, single-machine tool)

Each is defensible for a funded product and re-openable later; none serves v1.

| Dropped | Reason |
|---------|--------|
| **Windows Security Center / Action Center registration** | Modern WSC only recognizes third-party security products through a **gated Microsoft partner program** (protected-process, Microsoft-signed registration). It is not the simple COM-call-plus-registry-write the spec describes, and it's irrelevant to a tool you run for yourself. |
| **EV certificate + WHQL attestation (as a v1 requirement)** | Only needed to load a *kernel driver* on other people's machines. User-mode v1 doesn't need it; it returns only with the optional Phase-5 driver. |
| **Enterprise MDM / GPO / ADMX, WFAS `.wfw` import-export** | Fleet-management surface. A personal tool has no fleet. |
| **Federated learning + differential privacy (ε=1.0), cloud model CDN, canary/PagerDuty infra** | The vision statement says *no telemetry leaves the device*. Local-only means most of this machinery solves a problem the architecture says it doesn't have. Training is off-device on **your own** archived data. |
| **ARM64 / QNN NPU inference path** | The models (GBM/RF) run in <2 ms on a CPU EP. NPU acceleration is a micro-optimization for a non-problem at this scale. |
| **Separate `NGUpdater.exe`, and a heavy UI framework from day one** | A standalone auto-updater is out of v1. Note the **system-tray GUI (`ngtray`) is *in* scope** — it ships in Phase 2 and is required (a `LocalSystem` service can't show UI; session-0 isolation forces a separate interactive-session process). What's dropped is committing to a heavy framework up front: the tray starts minimal (icon + toast prompts + panic) and grows a dashboard in Phase 3, implemented in whatever is least effort (C#/.NET recommended). |
| **10 Gbps throughput target** | See D-3. |

---

## Kept from the original spec

WFP sublayer + weight ordering above Defender · tiered fast/slow decision split ·
SPSC ring buffer (returns in Phase 5) · ONNX Runtime for on-device inference · SQLite
policy store · HMAC hashing of sensitive log fields · coexistence-by-layering rather
than ripping Defender out. The vision was sound; the corrections are about sequencing
and the point-of-decision, not direction.
