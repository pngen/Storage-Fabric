# Contributing to Storage Fabric

Thank you for your interest in contributing. Storage Fabric is a
vendor-neutral, open-source C++20 runtime for governed AI storage
placement.

## Scope

Storage Fabric governs how AI runtime state and artifacts are placed,
moved, persisted, replicated, restored, validated, and retired across
heterogeneous storage tiers. It is not a filesystem, a database, or a
checkpoint format, and it does not attempt to replace them. It sits
above them through clean backend contracts.

## Ground rules

- C++20 is required. Prefer strong types, deterministic behavior,
  guarded lifecycle transitions, bounded parsing, and bounded allocation.
- Keep the portable core dependency-free where practical. CUDA is an
  optional, opt-in proof and must never be required by ordinary consumers.
- All generation comparisons must be explicit. A higher generation from
  a stale worker boot must never fence a fresh process incarnation.
- Unknown values must remain UNKNOWN. Do not claim NVMe, durability, or
  GPUDirect Storage without a real backend and proof.
- Do not fabricate physical storage tiers or cloud backends. Use real
  local measurements where possible and deterministic synthetic backends
  explicitly labeled provenance=SYNTHETIC otherwise.

## Building

See the README for the canonical build. The project targets MSVC 19.44,
CMake, and Ninja, and builds with /W4 /WX.

## Testing

Run the CTest suite in both Release and Debug configurations. Tests must
not rely on timeouts or watchdogs. Prefer deterministic, bounded fixtures
and real local-storage and multiprocess proofs.

## Contributions

- Use neutral, public-facing commit subjects (for example
  "feat: implement tier-aware placement").
- Do not embed prompt, harness, or internal workflow directives in commit
  messages.
- The working tree must be clean and all tests must pass before a release.

## License

By contributing, you agree that your contributions are licensed under
the Apache License 2.0 with copyright ownership by Summon Software Labs.
