# Storage Fabric

**Storage Fabric** is a governed, deterministic storage-placement runtime for
AI-produced state. It answers a systems question that is easy to pose but hard
to get right:

> Given a set of *logical objects* that a machine produces (checkpoints,
> shards, tensors, KV/prefix state, execution graphs), where should each one be
> stored, how many copies must survive, how does it move, and how can a reader
> know that what it gets back is exactly what a committed writer produced —
> without ever treating a filesystem path as an identity?

Storage Fabric separates **logical object identity** from **physical storage
path**. The same logical object may be published to many placements across many
tiers; identity is content- and governance-addressed, never path-addressed. A
storage placement is *governed state*: it records provenance, authority, a
generation, an integrity digest, durability intent, and a freshness/health
observation, so decisions about where data lives are explainable and
recomputable.

The runtime is written in C++20, builds cleanly under `/W4 /WX` (and the
POSIX equivalents), avoids exceptions on ordinary failure paths in favour of
`Status`/`Result<T>`, and exposes a small, strongly typed API.

---

## The systems question

Distributed and AI-at-scale storage is usually described in terms of *where
bytes are written*. Storage Fabric takes a different cut: it treats **where to
put a logical object** and **how many copies are durable** as a *decision* that
must be reproducible, auditable, and fenced. The planner does not use a single
opaque "master score". It applies named, weighted **soft factors** over a set of
hard **constraints**:

- hard constraints reject a candidate outright (backend unavailable,
  insufficient capacity, object too large, durability not available on a
  non-persistent tier);
- soft factors rank the survivors (locality match, read latency, write
  bandwidth, cost class, free-capacity headroom);
- tie-breaking is deterministic by *(score, backend id)* so the planner is an
  order-preserving function of its inputs, not of wall-clock noise.

Every factor carries an *origin* — MEASURED, REPORTED, DERIVED, SYNTHETIC,
UNKNOWN — so a number is never silently presented as a measured fact when it is
actually a modeled assumption.

## The AI-specific storage boundary

Machine-produced state is not user files. It has distinct durability, restore,
and movement semantics. Storage Fabric models these directly:

- **Object kinds** for shards, checkpoints, tensors, KV/prefix state, compiled
  artifacts, execution graphs/plans, adapters, dataset shards, and generic AI
  state.
- **Durability requirements** naming how many authoritative replicas must
  survive (a durability of zero is treated as optional/ephemeral).
- **Restore priorities** (CRITICAL, HIGH, NORMAL, LOW, BACKGROUND) so a restore
  plan can be ordered by consequence.
- **Retention policies** (PINNED, PROTECTED, TTL, LRU_CLASS, RECOMPUTABLE,
  DURABILITY_REQUIRED).
- **Locality preferences** and a bounded **compatibility metadata** map on each
  object descriptor.

## Object identity

An `ObjectDescriptor` carries a strong `ObjectId`, an
`ObjectGeneration`, a `ContentDigest`, an owner, provenance, a durability
requirement, a restore priority, and a retention policy. Identity is **not** a
path: the same digest appearing under two different object ids is expected
(content-addressed dedup), and the same id can legally have many placements.
The `01_object_identity` example shows two objects with identical bytes having
the same digest but distinct identities.

## Placements

A `PlacementRecord` is a governed claim of a specific copy: which backend,
tier, replica, manifest, key, digest, lifecycle, freshness, authority
generation, provenance, writer boot, and placement generation. A placement
becomes authoritative only in the `AVAILABLE` lifecycle state, reached through
the verification/commit path. Lifecycle transitions are guarded
(`can_transition`).

## Replicas

A `ReplicaSet` tracks, per object, the required and actual counts, the
distinct failure domains, the number of authoritative replicas, and a computed
state (HEALTHY / UNDER_REPLICATED / DEGRADED / REBUILDING / STALE / FAILED). A
stale replica never becomes authoritative just because its bytes still exist;
rebuild produces a fresh `ReplicaGeneration`/`PlacementGeneration`.

## Tiers

A `StorageTier` is a *class of storage* described by capability observations:
capacity, read/write latency, read/write throughput, concurrency, durability
class, evictability, persistence, freshness, and health. Every numeric
observation records how it was obtained (`MeasurementKind`). A tier never
*asserts* a physical device type unless it was measured or detected — unknown
properties remain UNKNOWN. The data path never trusts a backend name; it trusts
a descriptor.

## Manifests and chunks

Logical objects are split into content-addressable chunks and described by a
`Manifest`, which deterministically orders chunks by offset and stores each
chunk's digest and the physical blob it maps to. Manifest validation rejects
gaps, overlaps, duplicate chunk ids, non-covering manifests, offset overflow,
total-length mismatch, and stale chunk generations. The `04_chunk_manifest`
example publishes a chunked object, reassembles it, and validates both a valid
and a gapped manifest.

## Integrity

Integrity is cryptographic and content-addressed. SHA-256 is the primary
content-identity digest; CRC-32 is used for framing and persistence integrity
(never as the sole identity of a large object). `verify()` recomputes the
digest of stored bytes and compares it to the recorded digest, incrementing
`integrity_failures` on mismatch. The `03_integrity_verify` example publishes
an object, verifies it clean, then corrupts one physical blob byte on disk and
shows `verify()` failing with `DigestMismatch` while the unchecked `read()`
still returns bytes.

## Deduplication

Publication is content-addressed: an identical chunk digest is mapped to an
existing physical blob and its refcount is incremented rather than writing a
second copy. The `05_dedup` example publishes two logical objects with the
same bytes and shows `physical_blobs == 1` while `logical_objects == 2`,
with a single blob file on the local backend.

## Transactional publication

`publish()` and `publish_to()` are stepped: reserve capacity -> write chunks
via the backend -> eager-verify each chunk's digest -> sort and validate the
manifest -> record the placement -> commit the reservation. A reservation guard
releases the reservation if publication fails before commit, so capacity
accounting cannot leak. Dedup and chunking are configurable per publish.

## Movement

`replicate()` copies an authoritative placement to another backend.
`move()` does the same and then demotes the source placement to `STALE`
(movement, not eviction) so the old copy is no longer authoritative.

## Restore

A restore reads from an authoritative, digest-verified replica first.
`explain_restore()` and the object's `RestorePriority` are how a restore
plan is ordered; the `10_restore_priority` example shows CRITICAL, HIGH,
NORMAL, and BACKGROUND objects.

## Eviction

Eviction is guarded. Removing the last authoritative copy that still satisfies
durability is refused (`EvictionUnsafe`). A stale (non-authoritative) copy can
be evicted freely. The `09_eviction` example shows the sole-copy refusal, then
a `move()` to a second backend, then evicting the now-stale source while the
remaining authoritative copy is still protected.

## Capacity and reservations

`ReservationLedger` and `ReservationGuard` provide atomic, per-backend
capacity accounting that rejects overcommit, duplicate reservation, stale
release, double release, and any accounting that would go negative.

## Authority and generations

Every mutating action is described by an `AuthorityEnvelope` — the triple
`(CoordinatorEpoch, WorkerBootId, AuthorityGeneration)` plus an origin.
Authority compares lexicographically over that triple, so a **stale process
incarnation cannot fence a fresh one** even if it carries much larger local
generation numbers: the fresh incarnation's newer epoch/boot always orders
first. `new_coordinator_epoch()` advances the live epoch, and publication
records the accepted authority generation on the placement. The
`13_stale_authority` and `14_multiprocess_storage` examples demonstrate this
fencing.

The runtime as of this snapshot also rejects a mutation carried under a
strictly older authority than the live one (see `publish_to`), and records
`stale_rejections` in accounting.

## Backend contracts

A `StorageBackend` owns the physical-copy/IO side of a placement. Backends
adhere to a governed-key contract: keys are validated (never absolute, never
`..`, never escaping the backend root), and the backend never treats a path as
an identity. The local backend writes through temp -> flush -> atomic rename.
Backends report capacity, health, freshness, provenance, and capability flags.

## Real local storage proof

The `LocalBackend` is a real filesystem implementation. It uses a dedicated
root directory, validates every governed key, writes to a temp file, flushes to
disk where the platform allows it, and atomically renames into place. The
examples register a real local backend under a temp directory, publish objects,
read them back byte-for-byte, and verify their digests. This is genuine,
physically validated local storage — not a model.

## Synthetic remote-tier proof

Where a remote/object/shared/distributed tier is not genuinely available, the
`SyntheticBackend` models one in memory with explicit `SYNTHETIC`
provenance, a capacity ceiling, asymmetric read/write behavior, and
degraded/unavailable states. It is deterministic, honest about its origin, and
never claims to be a real physical tier. The `07_tier_placement`,
`08_storage_planner`, and `11_backend_health` examples exercise it and label
the results SYNTHETIC.

## Multiprocess proof

The `14_multiprocess_storage` example runs an in-process coordinator and a
worker thread over a real `TcpListener`/`TcpChannel` on `127.0.0.1`,
exchanging framed protocol messages (`encode_frame`/`decode_frame`). The
worker performs a mutation under a fresh authority (accepted) and then one
under a stale authority (fenced with `StaleAuthority`), printing the fencing
result. It does not exec an external CLI.

## CUDA staging proof

The `15_cuda_staging` example is written for an NVIDIA RTX 5090. All CUDA code
is guarded by `#if defined(__CUDACC__)`, so the normal C++ build compiles the
example and prints `LOCAL_STORAGE_TO_CUDA_STAGING`,
`CUDA staging proof skipped (not compiled with CUDA)`, `DIRECT_STORAGE=UNKNOWN`,
and `NOT GPUDirect Storage`. When compiled with `nvcc`, it creates a
deterministic object in Storage Fabric, persists it on a local backend,
restores/reads it, allocates device memory (`cudaMalloc`), copies H2D
(`cudaMemcpy`), runs a real add+xor transform kernel, copies D2H, and compares
against a CPU reference.

## Persistence and recovery

The runtime exposes a versioned binary metadata snapshot with a header, a
semantic SHA-256 digest, and a CRC-32, plus `save()`/`recover()`.
*Important:* the current snapshot does **not** round-trip its own metadata blob —
`deserialize_snapshot` rejects the bytes produced by `serialize_snapshot`
with `TrailingGarbage` / "metadata size mismatch", because the semantic digest
is written with a length prefix while the parser expects a fixed 32 bytes. This
is a real defect in this snapshot, not a data-loss event: the object bytes
remain on the local backend, and the intended recovery semantics (a fresh
coordinator epoch, downgraded `REVALIDATION_REQUIRED` backend freshness,
`participant_restarts + 1`) are gated behind a successful recover. The
`12_persistence_recovery` example demonstrates the serialize/deserialize and
save/recover path and reports the exact failure.

## CLI

The CLI is a separate tool executable built from `tools/`. It is a consumer
of the runtime and is intentionally not used by the examples, benchmarks, or
the downstream package proof, so those demonstrate the real library API rather
than shell execution.

## Examples

`examples/` contains fifteen runnable programs (one executable per file, built
from the CMake `examples/*.cpp` glob). Each registers a real local backend,
defines an object, publishes it, reads it back, and prints provenance,
freshness, tier, generation, digest, durability, and/or authority where
relevant.

- `01_object_identity` – identity is not path identity; content-addressed
  digest vs distinct ids.
- `02_local_publish` – publish/read/verify on a real local backend.
- `03_integrity_verify` – verify clean, then corrupt a blob and show digest
  detection.
- `04_chunk_manifest` – chunked publish, reassembly, and manifest validation.
- `05_dedup` – content-addressed dedup across two logical objects.
- `06_replica_set` – replica set metadata and a cross-backend replicate.
- `07_tier_placement` – tier candidate ranking with named factors.
- `08_storage_planner` – the planner: ranking, hard constraints, determinism,
  replica-target diversity.
- `09_eviction` – last-copy protection, move to a second backend, evict the
  stale copy.
- `10_restore_priority` – restore priority as a first-class object property.
- `11_backend_health` – live backend health and degraded/unavailable handling.
- `12_persistence_recovery` – persistence/save/recover and the metadata
  round-trip defect.
- `13_stale_authority` – authority triple ordering and epoch fencing.
- `14_multiprocess_storage` – in-process coordinator+worker over framed TCP.
- `15_cuda_staging` – guarded RTX 5090 CUDA staging proof (or skip labels).

## Benchmarks

`benchmarks/bench_storage_fabric.cpp` is a single benchmark measuring
completed work with explicit units using `std::chrono::steady_clock`, a fixed
payload, and reported thread count, object size, and wall time. It reports
object-descriptor canonicalization (ops/s, us/op), SHA-256 (MiB/s), manifest
creation (ops/s), placement selection (ops/s), dedup lookup (ops/s), metadata
lookup (ops/s), reservation cycle (ops/s), local sequential write (MiB/s),
local sequential read (MiB/s), persistence serialize (ops/s + bytes),
persistence recover (ops/s), protocol encode/decode (ops/s), concurrent reads
(ops/s), and synthetic remote-tier access (us/op, labeled SYNTHETIC). local
measured I/O is reported separately from the synthetic tier results.

## Package consumption

A downstream project consumes Storage Fabric via the installed CMake package:

```cmake
find_package(StorageFabric CONFIG REQUIRED)
target_link_libraries(app PRIVATE StorageFabric::storagefabric)
```

The `consumer/` directory ships an independent `main.cpp` and
`CMakeLists.txt` that do exactly that against an installed prefix: create an
object descriptor, build a local backend, build a placement plan, publish a
small deterministic object, verify its digest, read it back, explain the
placement, evict safely or release, and finish with clean accounting.

## Limitations

Limitations are stated once, precisely:

- The **local backend is physically validated** (real temp-file + flush +
  atomic-rename I/O); other backends are not physically validated.
- **Remote, object, and shared tiers are synthetic** unless a genuinely
  available backend is connected; the deterministic `SyntheticBackend`
  provides them in memory with `SYNTHETIC` provenance.
- **No cloud durability claims are made** without a real backend behind them;
  durability values are governance intent, not an attestation.
- **No GPUDirect Storage claim**: the CUDA example stages via `cudaMemcpy`
  and reports `DIRECT_STORAGE=UNKNOWN` / `NOT GPUDirect Storage` unless a
  genuine GPUDirect implementation and proof exist.
- **Local filesystem measurements may include OS page-cache effects** where
  applicable; they are not claimed to be physical-disk throughput.
- **Unknown device/backend facts remain UNKNOWN** — the runtime never asserts a
  physical device type, throughput, or health without a measurement or a
  detection, and it labels synthetic observations as SYNTHETIC.
- **Discovered engine defects in this snapshot**: (a) the metadata snapshot
  does not round-trip (`TrailingGarbage` / "metadata size mismatch", see the
  persistence section); (b) `TcpChannel::recv_frame` decodes only the 16-byte
  header and so fails with `Truncated` for any frame that carries a non-empty
  payload, so the multiprocess example reads the full frame itself via
  `decode_frame` on the combined header+payload buffer. These are surfaced in
  the examples rather than hidden.

## Contact

Storage Fabric is developed by Summon Software Labs.

## License
Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
