# EIN Full-Path Graphics Relay

A ground-up graphics correctness feature built around an authoritative native full seed,
selective native refinement, thirteen memory coats, EIN capability bits, and OpenGL/GLSL.
It is reconstruction-first and fails closed; graph and optional LAN administrators are supporting
resource machinery.

This repository is the first standalone implementation line. Its C++20 core is backend-independent
and runnable now.
The GLSL 4.60 path classifies temporal continuity, requests native refinement for unproven pixels,
resolves only valid outputs, and refuses to promote an unresolved frame into history.

It occupies the same broad temporal-super-resolution problem space as features such as DLSS, but it
does not use NVIDIA code, models, names, or APIs. Its distinct purpose is proof-gated correctness:
uncertain work becomes native work rather than an unreported approximation.

## Status

- Implemented: EIN handles, C0–C12 memory policies, fence retirement, relay planning, proof gate,
  fault tests, temporal GLSL classification, native-refine masks, fail-closed resolution, and
  hierarchical massive classification units with compact indirect work queues. Trusted LAN nodes
  can advertise all thirteen coats and receive generation-bound massive-unit leases.
- Next boundary: the live OpenGL 4.6 resource/context adapter, native-refine tile renderer, and an
  application-selected authenticated carrier for LAN work payloads.
- License: MIT.

## Build and run

With a C++20 compiler:

```sh
make test
```

Or with CMake 3.24+:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The demo proves that the first frame is full/native, the next frame is reduced and relayed to native,
a failed proof schedules FULL, and a camera cut also returns to FULL. It prints the thirteen-coat
route, typed graph, memory usage, state transitions, and OpenGL barrier masks.
See `docs/architecture.md` for the contract and milestone sequence.

The LAN test runs the real UDP codec over loopback, then proves that discovery alone cannot assign
work and that missing, late, restarted, or unproven results return to local native processing.

## Correctness invariant

```text
FULL seed → relay candidate → native refinement → resolve → proof → history
                                                          └─ failure → FULL
```

History advances only when identity, depth, motion, tile coverage, and the preservation-error bound
all pass. See `docs/correctness.md` for the proof and fault-injection matrix.

## Layout

- `include/ein/graphics_shell.hpp` — EIN bits, memory coats, graph and administrator API.
- `include/ein/massive_classifier.hpp` — 16×16 leaf and 128×128 massive classification contracts.
- `include/ein/gpu_layout.hpp` — checked C++ mirrors of GLSL `std430` records.
- `include/ein/lan_protocol.hpp` — versioned node identity, feature, coat-capacity, and wire contract.
- `include/ein/lan_discovery.hpp` — cross-platform TTL-one UDP multicast discovery.
- `include/ein/lan_administrator.hpp` — trusted capacity selection, leases, result gate, and fallback.
- `src/graphics_shell.cpp` — allocation, generation, validation, barriers and fences.
- `src/massive_classifier.cpp` — deterministic reference classifier, queue rebalance and governor.
- `src/lan_*.cpp` — fixed packet codec, socket implementation, and LAN resource coordinator.
- `src/demo.cpp` — object/visibility/indirect-buffer vertical slice.
- `tests/path_relay_tests.cpp` — deterministic proof, discontinuity, refresh and coat tests.
- `tests/massive_classifier_tests.cpp` — hierarchy, queue, governor, and generation tests.
- `tests/lan_administrator_tests.cpp` — wire, socket, trust, lease, proof, and fallback tests.
- `shaders/lod_cull.comp` — frustum, projected-size LOD, visibility and indirect counts.
- `shaders/full_path_relay.comp` — strict temporal classification and native-refine mask.
- `shaders/full_path_resolve.comp` — fail-closed native refinement and proof counters.
- `shaders/classify_tiles.comp` — packet-to-16×16 proof and cost reduction.
- `shaders/classify_massive_units.comp` — 128×128 aggregation and queue compaction.
- `shaders/build_classification_dispatch.comp` — indirect arguments and frame-level escalation.
- `shaders/mesh.vert`, `mesh.frag` — indirect instance lookup and minimal shaded output.
- `docs/integration.md` — binding table, barriers and OpenGL execution sequence.
- `docs/classification-units.md` — hierarchy, layouts, queues, barriers, and governor.
- `docs/lan-administrators.md` — deployment, trust boundary, wire protocol, leases, and fallback.

The public application contract intentionally contains no raw OpenGL object names. OpenGL is a
replaceable device adapter under the administrator; applications remain connected through EIN
handles and graph ports.
