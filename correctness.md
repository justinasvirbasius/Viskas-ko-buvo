# Correctness protocol

## Authority

A native FULL frame is the authoritative seed. A relay frame may become the next history frame only
after a `PathProof` agrees with its `seed_generation` and establishes all of the following:

- complete EIN slot/generation identity coverage;
- complete depth coverage;
- complete motion coverage for relay frames;
- zero unresolved native-refinement tiles;
- maximum measured preservation error at or below the configured bound.

A full seed does not require prior-frame motion, but it still requires identity, depth, coverage, and
error validation.

## LAN work remains subordinate

A LAN result cannot advance C10 history directly. The local coordinator leases only a massive
classification or refinement unit and binds it to the remote node's boot generation, the local frame
and seed generations, the unit coordinates, and an input digest. Acceptance additionally requires
the coordinator to recompute the output digest, requested identity/depth/motion proof bits, zero
unresolved pixels, and error measurement locally. Remote proof claims are never authoritative, and
the ordinary frame-level proof gate still controls history.

Expiry, node restart, trust revocation, or any proof mismatch returns the original request through
the local fallback queue. A packet from an untrusted discovered node is never eligible for a lease.

## Failure behavior

Proof failure calls `invalidate()` and returns `false`. The next `plan()` returns `FullSeed` at native
resolution. Camera cuts and topology changes are latched the same way: discarding the returned plan
cannot accidentally make the old history valid again.

Expected discontinuities are not exceptions. Structural misuse—such as committing a proof from a
different seed generation—is an exception because it indicates an API sequencing defect.

## GPU proof sequence

1. `full_path_relay.comp` validates history identity/depth/motion and writes a relay candidate,
   confidence surface, and native-refinement mask.
2. Native shading fills every requested pixel or tile and writes a validity mask.
3. `full_path_resolve.comp` selects proven relay pixels or valid native-refine pixels. Missing native
   work increments `unresolvedPixels`.
4. The error pass compares resolved output against scheduled reference samples.
5. C11 Readback carries proof counters and maximum error after a fence.
6. The CPU calls `commit(plan, proof)`. Only `true` permits the C10 history ping-pong swap.

## Fault-injection matrix

| Injected condition | Required result |
| --- | --- |
| EIN generation mismatch | native-refine mask; proof cannot accept unmatched history |
| One unresolved tile | `commit` returns false; next frame is FULL |
| Error above bound | `commit` returns false; next frame is FULL |
| Camera cut | old history invalidated immediately; FULL remains latched |
| Topology change | old history invalidated immediately; FULL remains latched |
| Low mean confidence | FULL scheduled |
| Refresh interval reached | FULL scheduled without treating it as a fault |
| Untrusted LAN advertisement | visible as advisory capacity; never receives a lease |
| LAN node restart or timeout | exact massive unit returned to local work |
| LAN frame/seed/digest mismatch | result rejected; exact massive unit returned locally |
| LAN result has missing proof or unresolved pixels | result rejected; local native fallback |
