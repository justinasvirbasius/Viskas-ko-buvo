# EIN Full-Path Graphics Relay — architectural accord

## Definition

The framework is first an **open full-path graphics relay**. It is closer in purpose to a temporal
super-resolution feature than to a network administrator. It is not an editor clone and it is not
named after any proprietary engine. A complete native-quality frame establishes the pathway;
later frames preserve that pathway from reduced-cost current samples.

The shell behaves like a graphics TPU at the scheduling level: SENSE → BUDGET → REFINE →
RECONSTRUCT is compiled into compute, transfer, raster, and presentation work. OpenGL/GLSL is the
first backend. The connectivity graph remains internal scheduling machinery, not the product's
identity.

## Full-path correctness rule

The full render is the authority. The first valid frame, a camera cut, a topology discontinuity,
low confidence, a failed proof, or a scheduled refresh runs **FULL** at native extent. That frame
seeds geometry identity, depth, motion convention, lighting frequency, exposure, and native history.

```text
FULL native seed
    ↓ preserve identity + depth + lighting frequency
SENSE motion / depth / reactive regions / disocclusion
    ↓
BUDGET pixels as relay / native-refine / full
    ↓
REFINE every unproven tile at native quality
    ↓
RECONSTRUCT native output
    ↓
PROVE identity + depth + motion + zero unresolved tiles + error bound
```

Budgeting is hierarchical: the frame contains 128×128 massive classification units; each unit
contains 64 established 16×16 tiles; each tile contains 4×4 packets and pixels. The massive level
aggregates proof pressure and compacts work without weakening leaf-level validation.

“Copy the pathway” does not mean blindly copying old pixels. History is accepted only where EIN
slot and generation, motion, depth, neighbourhood range, reactive masking, and seed generation
agree. Every rejected pixel is marked for native refinement. If even one required tile remains
unresolved, or measured preservation error exceeds the configured bound, the result cannot become
history and the next plan is FULL. Acceleration therefore fails closed.

## Boundary

```text
application / tool / game
        |  EIN handles + typed graph requests
EIN Graphics Administrator (userspace)
        |  validated resource, queue and surface operations
OpenGL backend / graphics driver
        |  mappings, fences, interrupts and device queues
kernel memory + PCI/DMA + display/input services
```

Applications never consume kernel tables or raw graphics-object names. The kernel supplies
mechanism; the graphics administrator owns graphics policy.

An optional LAN resource fabric may be attached below the classifier. Each node advertises spare
C0–C12 capacity and feature bits, but the presenting machine remains the FULL/history authority.
Only locally trusted node identities can receive short, generation-bound massive-unit leases.
Remote timeout, restart, or proof failure returns the exact work item to the local native path.
Discovery is TTL-one multicast and is never used as authentication; see `lan-administrators.md`.

## EIN bits

Every shared object uses one 64-bit capability word:

| Bits | Field | Purpose |
| --- | --- | --- |
| 0–19 | slot | index into the administrator's resource table |
| 20–31 | generation | rejects stale handles after retirement |
| 32–39 | kind | buffer, texture, shader, pass, surface, or session |
| 40–47 | coat | memory residency/lifetime class |
| 48–55 | rights | read, write, execute, connect, share, present |
| 56–63 | domain | application/session isolation boundary |

An EIN handle is identity plus authority. It is never a pointer and never a GPU virtual address.

## The thirteen coat-after-coat memory policies

| Coat | Policy | Main contents | Lifetime / governing rule |
| --- | --- | --- | --- |
| C0 Bootstrap | device foundation | capability tables, fallback resources, binding schema | device generation |
| C1 Frame | per-frame scratch | uniforms, constants, command assembly | current frame fence |
| C2 Upload | copy ingress | mapped staging blocks | until copy fence |
| C3 Stream | continuously renewed | vertices, instances, particles, dynamic glyphs | rotating multi-frame ring |
| C4 Geometry | durable shapes | vertex/index buffers, meshlets, LOD ranges | asset residency |
| C5 Texture | sampled fields | textures, LUTs, sparse pages, samplers' backing | asset/page residency |
| C6 Storage | general shader work | SSBOs, shader images, material and light tables | graph-declared |
| C7 Visibility | scene selection | bounds, LOD choices, Hi-Z and compacted records | view/frame generation |
| C8 Indirect | GPU-issued work | draw/dispatch commands and atomic counters | producer→consumer fence |
| C9 Attachment | raster products | color, depth, G-buffer and shadow targets | render-pass extent |
| C10 History | temporal continuity | TAA, motion, exposure and prior-frame fields | explicit N-frame chain |
| C11 Readback | observable return | timestamps, counters, captures and fault reports | GPU→CPU fence |
| C12 Recovery | successor preservation | resource recipes, hashes and compact CPU shadows | device reset/rebuild |

Movement between coats is explicit. Reuse is fence-gated. Each recycled slot receives a new
generation so an old application handle cannot accidentally address new content.

Memory relayering reserves the entire destination allocation while retaining the source, blocks
dependent transitions, and activates a same-slot/new-generation EIN handle only after its copy
fence. Target pressure preserves the source and emits fallback. A bounded shell-notice ring reports
scheduled/block, committed/proceed, and fallback phases without becoming allocator state itself.

The Full-Path executive adds a non-blocking schedule above these mechanisms. Caller-supplied
monotonic timestamps bound every frame and stage; an advancing completion token is required to keep
a stage alive. LAN and memory-relayer overruns leave the proof path through explicit local/source
fallbacks. Any proof-critical overrun invalidates the candidate and schedules FULL. The executive
does not sleep, and early completion returns time directly to the frame.

## Supporting connectivity contract

A supporting connection is `(source node, destination node, port kind, access)`. Port kinds are control,
memory, commands, pixels, and telemetry. The graph rejects absent endpoints, executable cycles,
illegal surface routes, and unshared cross-domain writes. This replaces hidden global renderer
state with inspectable scheduling while the Full-Path Relay remains the application-facing feature.

The initial GPU-driven route is:

```text
camera/object buffers
  → compute frustum + projected size + LOD
  → visible records + per-LOD indirect commands
  → SSBO/command barrier
  → indirect opaque draw
  → later: depth, Hi-Z, shadows and material passes
```

## Barrier and ownership rule

Every pass declares required state and access for every resource. The administrator compares the
previous state with the requested state and emits the backend barrier. The critical first path is:

```cpp
compute writes visible SSBO + indirect commands;
glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
glMultiDrawElementsIndirect(...);
```

No application may insert an untracked write between those operations. Destruction means
`ACTIVE → RETIRING(fence) → FREE(new generation)`, never immediate deletion.

## What this vertical slice implements

- packed and validated EIN capability handles;
- thirteen independent memory-coat arenas with aligned first-fit allocation and coalescing;
- generational retirement behind submission fences;
- domain ownership and capability checks;
- typed connectivity graph with cycle validation;
- declared resource uses and generated OpenGL barrier masks;
- a compilable trace demo for compute culling → LOD → indirect draw;
- GLSL 4.60 compute, vertex, and fragment interfaces for that path;
- a full-seed/relay state machine with camera-cut, topology, confidence, proof, and refresh invalidation;
- exact EIN slot/generation continuity tests for temporal samples;
- a relay classification shader that produces confidence and native-refinement masks;
- a fail-closed resolve shader that counts unresolved, relayed, and natively refined pixels;
- a CPU proof gate that refuses history when identity, depth, motion, tile coverage, or error bounds fail.
- 128×128 massive classification units over 8×8 groups of 16×16 leaf tiles;
- compact Relay, Selective Refine, Native, and Full queues with indirect dispatch arguments;
- ALU, memory, branch, texture, overdraw, and post cost aggregation plus saturation telemetry.
- versioned LAN coat-capacity discovery, explicit trust, massive-unit leases, proof gating, and
  deterministic local fallback.
- fence-bound memory relayering with canonical EIN rollover and bounded shell-notice telemetry.
- transactional submissions that publish neither partial state nor a fence on validation failure;
- deadline-aware frame execution with progress watchdogs, exception containment, and FULL cooldown.

The trace backend makes the contracts testable without requiring a window system. It deliberately
does not pretend that a GPU object was created.

## Next implementation milestones

1. **Live Full-Path correctness pass** — bind current color/depth/motion/reactive/identity plus
   ping-pong history, dispatch `full_path_relay.comp`, natively shade its refine mask, dispatch
   `full_path_resolve.comp`, and accept history only when `unresolvedPixels == 0`.
2. **GL 4.6 device adapter** — DSA buffer creation, persistent mapped C1/C2/C3 rings, sync objects,
   debug labels, and capability probing.
3. **Shader administrator** — compile/link cache, include graph, reflection, fixed binding schema,
   hot replacement by generation, and error isolation.
4. **Live GPU-driven draw** — initialize three indirect commands, dispatch `lod_cull.comp`, issue
   the exact barrier, and call one multi-draw.
5. **Surface and compositor route** — window surfaces become graph nodes; multiple app sessions
   submit layers without owning the device context.
6. **Recovery** — retain C12 recipes, rebuild C4–C10 coats after reset, and publish degraded
   state without invalidating the shell service.
7. **Secure LAN work carrier** — bind the implemented leases to an application-selected
   authenticated and encrypted payload transport without giving discovery packets authority.

The smallest next code step is milestone 1 only: execute one native seed, one 0.58-scale relay,
and one selective native-refine pass. Deliberately leave one tile unresolved and verify that the
proof gate rejects the history swap and schedules FULL.
