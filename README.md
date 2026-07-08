![Splinter Header](https://splinterhq.github.io/design/splinter-readme-hero-dark.png "Think of it like semantic breadboard.")

# Splinter — A Cooperative Userspace Hypervisor that merges Inference, KV, Vectors, Graph & Governance in the same memory lane as the socket.

Welcome! Splinter doesn't quite have a reference point that describes the class of tool it belongs to; it does quite a few remarkable things. To understand it, we have to _quickly_ and _briefly_ touch on why it was created. But first, BADGES!

![EAGAIN is a signal](https://splinterhq.github.io/design/badges/eagain-is-a-signal.png "Feel free to use this badge if you use Splinter!") | ![Powered By Splinter](https://splinterhq.github.io/design/badges/powered-by-splinter.png "Feel free to use this badge if you use Splinter!") | ![Built With Splinter](https://splinterhq.github.io/design/badges/built-with-splinter.png "Feel free to use this badge if you use Splinter!")

## Why Splinter?
Local Large Language Model (LLM) inference is currently choking on the "Socket and Lock" tax. Standard IPC tools and databases require heavy context switching, serialization, and kernel interrupts just to synchronize state. When you are generating text or evaluating semantic alignment at token speeds, that overhead isn't just a bottleneck — it's a wall.

Splinter dismantles that wall. It is a lock-free, cooperative userspace hypervisor built from the ground up for strict mechanical sympathy with modern CPU cache hierarchies (x86_64, ARM, and RISC). It puts your governance, your vector storage, and your inference engine in the exact same physical memory lane.

Think of Splinter as a **semantic breadboard**: a passive, shared-memory manifold where thousands of context or classification windows for multimodal inference run simultaneously with 100% non-blocking throughput. There is no central arbiter, no socket, and no copy — reads and writes happen at L3 cache speeds, directly on the hardware bus.

If this is your first stop, you're welcome here. Splinter is open source, actively researched, and built in the open. Whether you want to embed the library, drive the CLI, extend the toolchain, or just understand how a lock-free shared-memory substrate holds together, there's a place for you. See [**Collaborating**](#collaborating) below.

## Quick Install

On **Debian / Ubuntu** (and derivatives), the Big Bang installer builds and installs the full semantic stack (Splinter with embeddings, llama.cpp, and Lua) from source. This will install all OS package dependencies needed to build splinter and its dependencies from source (takes ~15 minutes on average systems):

```sh
curl -fsSL https://raw.githubusercontent.com/splinterhq/libsplinter/main/scripts/bigbang.sh | bash
```

Sources are checked out under `/usr/local/src` so you can `git pull` and rebuild them later. For other platforms, or to choose your own feature flags, see [**Building**](#building) below.

## What Splinter Gives You

Every process that maps a Splinter store gets the same primitives, with no daemon in between:

- **Key/value storage** — a fixed-geometry, memory-mapped hash store with FNV-1a keys.
- **Vector & embedding storage** — per-slot embeddings with semantic similarity/distance search (optional build).
- **KV & cache residence** — persistent on disk or purely in-memory, your choice at creation.
- **Atomic integer operations** — in-place `AND`/`OR`/`XOR`/`NOT`/`INC`/`DEC` on `BIGUINT` keys, Redis-style but with no round trip.
- **Ordered sets** — tandem keys (e.g. `car`, `car.1`, `car.2`) for storing related vectors or physics-style ordered data under one base key.
- **Named slot types** — `VOID`, `BIGINT`, `BIGUINT`, `JSON`, `BINARY`, `IMGDATA`, `AUDIO`, `VARTEXT`.
- **Bloom labels** — a 64-bit label mask per slot for O(slots) semantic routing and batch enumeration (not a hot-path index — routing, not search).
- **Pub/sub notifications** — 64 signal groups you can poll, plus an `eventfd`-backed event bus with a per-slot dirty mask so watchers wake only on what actually changed.
- **Cooperative memory advisement** — a 32-slot Logic Shard bid table where processes declare `madvise` intent and elect a sovereign, so independent `posix_madvise()` calls never thrash the page cache.
- **Self-elected univocality** — governance of shared regions via POSIX advisement primitives, with no lock and no arbiter; every process computes the same sovereign from the same static bid data.
- **Universal FFI reach** — because the store *is* the ABI, any language with a C FFI joins the same manifold as a first-class peer. Splinter ships TypeScript bindings for **Bun** and **Deno** and native **Rust** crates (in-memory and persistent), so a Node-runtime agent, a Rust service, and a C inference engine can all map the same store and see each other's writes at L3 speed. There's no client/server asymmetry — every process on the system is a universal option.
- **A production-grade CLI/REPL** — for debugging, operations, ingestion, and inspection.

All of it stays small enough to live in the CPU hot path, and none of it requires copying between services.

## The Core Concept: Cooperative Virtualization

Splinter treats your local workspace not as a database pipeline, but as an execution topology:

- **The Privilege Topology** — When run as `root`, Splinter is **ringless**, bypassing standard OS arbitration barriers to align directly with CPU L1/L3 cache lines and pin NUMA nodes. Under an unprivileged user it acts as a **single-ring** virtualized environment, managing its own ecosystem of agents and shards without trapping down into Ring 0.
- **Cooperative Scheduling** — Rather than aggressive, preemptive hardware interrupts that cause CPU thrashing, Splinter coordinates access via an aligned 32-slot bid table. Shards check a shared sovereignty table and voluntarily yield memory regions using a protocol backed by POSIX `madvise` primitives.

## The One Invariant: The Seqlock

Splinter has no protection layer between processes, so it relies on one rule that every reader and writer honors. Each slot carries a 64-bit **epoch** counter:

- **Even epoch** — the slot is stable, safe to read or write.
- **Odd epoch** — a writer is active; the slot is in transition.

Read the epoch before you touch a slot, read it again after, and if it moved, the read was torn — retry. That's the entire seqlock protocol; readers never block writers, and L3 cache residency stays pristine.

`EAGAIN` is therefore a **signal, not an error**: the slot is momentarily contested, so back off and try again. (`splinter_retrain_slot()` is the one sanctioned exception that drives an epoch *backward* to republish a scrubbed slot — your `e1 != e2` check catches it correctly.) The public header, `splinter.h`, opens with a full **AI Primer** that lays out the invariants, the risk topology of every API call, and the operational geometry — it's worth reading before your first write, whether you're human or an agent.

## Signals & Pub/Sub

Splinter coordinates processes without ever having one notify another directly. It exposes **64 signal groups** (0–63), each a single atomic counter living in the shared header. A write doesn't push to subscribers — it pulses a counter:

```
write → splinter_pulse_watchers() → signal_group counter increments
```

There are two ways to consume those pulses, and you can mix them:

- **Poll the counter (the floor).** `splinter_get_signal_count(group_id)` is your heartbeat: when the count changes, scan the slots subscribed to that group for moved epochs. Subscribe a key with `splinter_watch_register(key, group_id)`, or subscribe by label with `splinter_watch_label_register(bloom_mask, group_id)` so an entire semantic class wakes together. This path needs no syscalls and works anywhere.
- **Block on the event bus (kernel-assisted).** The owner process arms an `eventfd` once with `splinter_event_bus_init()`. Any process then calls `splinter_event_bus_open()` and blocks in `splinter_event_bus_wait(fd, timeout_ms)` until a write advances the global epoch. On wake, `splinter_event_bus_get_dirty()` hands you a bitmask of exactly which slot indices changed, so you rescan only what moved instead of sweeping the whole store. The fd is a normal pollable descriptor, so it drops cleanly into an existing `poll`/`epoll` reactor alongside your other I/O.

Prefer the event bus whenever you can afford to block — it trades a busy spin for a clean kernel wake. Bloom labels make this expressive: a client can set a `WAITING` label, a sidecar enumerates matches and transitions it through `SERVICING` to `READY`, and consumers wake on the corresponding signal group. Governance observes the bloom directly, so the whole handshake stays in shared memory with no broker in the middle.

## Architectural Features

- **Zero-Copy Substrate** — Multi-dimensional arrays mapped via `mmap` are treated as raw, continuous memory lanes. Client-side WASM engines (via WASMEdge) or local Lua scripts execute fixed-width SIMD instructions directly over the shared pointers without intermediate serialization.
- **Mechanical Sympathy** — A lock-free, 64-byte-aligned architecture using sequence locks (inspired by the Xen hypervisor) ensures readers never block writers, maintaining pristine L3 cache residency.
- **In-Place Atomics** — Execute bitwise and mathematical operations directly on `BIGUINT` keys within the shared memory pool.
- **The Mop** — A configurable scrubbing mode (off / hybrid / full boil) keeps stale bytes beyond a slot's live length from leaking into raw reads, which matters for LLM memory, forensics, and verifiable research.
- **Externalized Hippocampus** — AI Agents can dynamically spin up local semantic scratchpads in `.splinter/` to offload raw context retrieval from their expensive API windows, dropping token consumption by up to 45% while driving down hallucination rates.

## The Splinter Toolchain

Splinter ships with a minimalist, bare-metal C toolchain for production-grade telemetry and control:

- **`splinference`** — An embedding inference engine that maps directly to the bus with no socket layer, managed natively by `systemd`.
- **`splainference`** — Demo concept of a completion and conversational runtime that maps system prompts, generation windows, and active RAG contexts directly to the shared substrate. An example for `llama.cpp` integrators.
- **`splinterctl` & `splinterpctl`** — A lightning-fast CLI and REPL that completely isolate administrative interaction from core storage performance.
- **`sidecar`** — A DevOps monitoring tool giving real-time visibility into the semantic bus, tracking active slots, bid windows, and arbitrary debug chatter.

### The CLI at a Glance

`splinterctl` runs one-shot or as a REPL. A representative slice of what's built in:

| Command | Purpose |
| --- | --- |
| `use` / `init` | Open a store by name or path; initialize new stores. |
| `set` / `get` / `unset` / `append` | Core key/value operations. |
| `head` / `list` / `type` / `config` | Inspect metadata, keys, slot types, and bus/slot configuration. |
| `math` | `incr`/`decr` and bitwise ops on named `biguint` keys. |
| `orders` | Manage ordered vector sets (tandem keys) for a base key. |
| `label` / `bind` | Set bloom-filter labels and bind them to signal-arena groups. |
| `watch` / `bump` | Observe a key for changes; bump a slot's epoch on demand. |
| `shard` | Inspect or seed the Logic Shard bid table (cooperative advisement). |
| `retrain` | Zero a key's vectors and flip its epoch back to republish it. |
| `search` / `ingest` | Semantic similarity search and chunked file/stdin ingestion *(embeddings build)*. |
| `wasm` / `lua` | Run WASM via WASMEdge, or a Lua script *(optional builds)*. |
| `export` | Export store data in various formats. |
| `caps` / `uuid` / `hist` / `help` | Show compiled-in capabilities, generate UUIDv4s, manage history, get help. |

## Building

On Debian/Ubuntu, `scripts/bigbang.sh` will build and install Splinter with vector storage and `llama.cpp` from source, then install both on your system. It also downloads Nomic Text 1.5 from Hugging Face for you — which is all you need to get started with Splinter as a semantic breadboard.

For the advanced features (Lua, WASMEdge, NUMA), run `./configure --help` to see how to enable them. A `--install-deb-deps` option will automatically install the Debian packages needed to build Splinter's optional dependencies.

A typical build:

```sh
./configure --with-llama --with-vectors --with-lua --with-wasm --with-numa
make
sudo -E make install
```

`make tests` runs the unit suite under Valgrind if it's installed on your system. See `splinter_chi_sao` for the collision-resolution benchmarks. There's a deeper architectural treatment in `splinter_thesis.pdf`, and full API docs live at <https://splinterhq.github.io>.

## Performance Under Fire

Tested rigorously under strict hardware constraints (including fanless, low-tier Chromebook development environments):

- **Multi-Reader, Single-Writer (MRSW)** — sustains **3.2 million operations/second** with zero data corruption.
- **Multi-Reader, Multi-Writer (MRMW)** — using the disjointed-lane collision-resolution protocol (`splinter_chi_sao`), 32 concurrent writers sustain **15.6 million operations/second** with zero data corruption.

On Tiger Lake i3 hardware this lands around ~937 cycles/op; on Zen 4/5 with the manifold pinned to L3, projected cost drops to single digits. The instruction path is kept deliberately minimal so the hot path fits in the CPU instruction cache.

## What Splinter Is *Not* For

Knowing the edges keeps everyone out of trouble:

- Multi-machine replication — use a real database.
- Durable ACID transactions — the seqlock is crash-consistent, not ACID.
- Dynamic schema evolution at runtime — geometry (slot count, max value size) is fixed at creation.
- High-cardinality keyspaces without a pre-planned slot count — there is no eviction.
- Any workload where you cannot tolerate `EAGAIN` and retry.

## Collaborating

Contributions of every kind are genuinely welcome — code, docs, benchmarks, bindings, bug reports, and grounded "oddball" ideas. A few things keep the project healthy:

- Add tests when new behavior needs them, and make sure existing tests pass.
- Unit tests should run **Valgrind clean**.
- For anything beyond trivial, be willing to help maintain it — and if it's outside the current development focus, open an issue to discuss first.
- Ideas don't have to be conventional, but they should be grounded in real work you're doing or trying to do.

Please read `CODE_OF_CONDUCT.md` and `CONTRIBUTING.md` before opening a pull request. **One important note on pace:** the maintainer is a brain-cancer survivor doing active research, so PRs may surge forward and then go quiet for a week or more. That's normal here. The maintainer isn't reachable via chat or social platforms (including Discord) — GitHub issues and pull requests are the way in. It's nothing personal; it's just how the work gets done well.

## Governance and Open-Source Commitment

Splinter is open source because transparent, text-driven governance and systemic bias auditing should be fundamental infrastructure, not a locked enterprise feature.

Once version 1.2.0 ships, the core repository moves into long-term **maintenance mode**: continuous updates, performance optimizations, and community-driven enhancements and fixes to existing features, but a broadly locked feature set. Splinter can't keep its original identity and also be a kitchen sink. As it stands now, Splinter is one of the most lean, efficient and deliberately-designed things in semantic infrastructure. Every single thing has a well-defined place and purpose. Adding literally *anything else* could compromise that.

After 1.2.0, further additions would be so specific [to the author's use case](https://foreshock.io) that few others would benefit. Most of the commercial code is also free software (like inference engines for models built on splinter as a file format) — just no longer part of *this* project.

## License

Apache 2.0 (MIT available on request). See `LICENSE.txt`.
