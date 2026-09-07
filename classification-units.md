# Massive classification units

## Hierarchy

The classifier does not replace the 16×16 leaf-tile contract. It adds a large aggregation level:

```text
frame
  → massive unit: 128×128 pixels
    → 8×8 leaf tiles
      → leaf tile: 16×16 pixels
        → 4×4 packet grid
          → packet: 4×4 pixels shared by a 2×2 lane quad
            → each lane evaluates one 2×2 microcell
```

One massive unit therefore represents as many as 64 tiles and 16,384 pixels without requesting an
illegal 16,384-thread workgroup. The leaf pass uses 64 threads; the massive pass uses another 64.

At 3840×2160 the grid contains 32,400 leaf tiles and 510 massive units. A full `GpuTileRecord` is
80 bytes, so leaf evidence occupies about 2.47 MiB before queue storage. Massive records add about
32 KiB.

## Resolution lanes

| Lane | Meaning | Required action |
| --- | --- | --- |
| Relay | every relevant proof is stable | reuse the verified pathway |
| Selective Refine | a bounded subset is uncertain | shade only marked native pixels/tiles |
| Native | sparse work would no longer be efficient or sufficiently stable | shade the whole 16×16 tile |
| Full | seed/input proof is invalid | abandon relay and render a native FULL frame |

The C++ reference classifier and GLSL leaf classifier use the same default thresholds. Invalid
numbers, missing samples, and seed-generation mismatches classify as FULL. Queue overflow never
drops work: selective overflow is promoted to Native; an exhausted safe queue escalates to FULL.

## Cost classes

Each leaf records raw pressure in six domains: ALU, memory, branch, texture, overdraw, and post.
The massive pass sums these values and exposes the dominant cost. This does not decide correctness;
it selects the most suitable refinement kernel after correctness has already chosen the resolution
lane.

Examples:

- texture-dominant selective tiles can use coherent neighbourhood gathers;
- branch-dominant units can be split to reduce divergence;
- overdraw-dominant native units can prefer depth-first refinement;
- saturated units expose pressure instead of silently overflowing a queue.

## GPU pass sequence

```text
full_path_relay.comp
  → confidence + native-refine mask
  → classify_tiles.comp
  → 16×16 TileRecords
  → classify_massive_units.comp
  → 128×128 unit records + compact lane queues
  → build_classification_dispatch.comp
  → four indirect dispatch commands + batch governor
```

Required OpenGL barriers:

```cpp
glDispatchCompute(relayGroupsX, relayGroupsY, 1);
glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                GL_TEXTURE_FETCH_BARRIER_BIT);

glDispatchCompute(tileGridX, tileGridY, 1);
glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

glDispatchCompute(massiveGridX, massiveGridY, 1);
glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

glDispatchCompute(1, 1, 1); // build indirect arguments
glMemoryBarrier(GL_COMMAND_BARRIER_BIT |
                GL_SHADER_STORAGE_BARRIER_BIT);
```

The queue and summary buffers must be cleared before the first classification dispatch. Queue task
records and indirect commands use a 16-byte stride. Matching C++ POD layouts live in
`include/ein/gpu_layout.hpp` and are guarded by static size assertions.

## Batch governor

The massive pass publishes queue depth, overflow, saturation, native-equivalent pixels, dominant
cost, hot execution lane, and rebalance count. The final governor schedules FULL when any unit has
an invalid proof, a safe queue exhausts its capacity, or the native-equivalent pixel ratio reaches
the configured frame threshold (0.72 by default).

This final threshold is an efficiency decision with a correctness-safe direction: it may request
more native work, never less.
