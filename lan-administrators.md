# LAN graphics administrators

The LAN layer is an optional resource fabric beneath the EIN Full-Path Relay. Every participating
machine may run a small graphics administrator and advertise spare capacity, including the
available bytes in all thirteen coats. The local coordinator may then lease one established
128×128 massive classification unit at a time.

The LAN does not become the correctness authority. The machine presenting the frame retains the
FULL seed, the history generation, the proof gate, and the final decision to advance history.

## Resource path

```text
local FULL seed and frame generation
              |
      massive unit request
       /               \
local GPU        trusted LAN lease
                       |
          classify or native-refine
                       |
             generation + digest
             + complete proof
                       |
             local proof and resolve
                       |
        invalid / late / missing result
                       |
             local native fallback
```

Remote work is limited to `ClassifyMassiveUnit`, `SelectiveRefine`, and `NativeRefine`. There is no
remote FULL work kind: a remote result is a work product, never a replacement authority or a new
history seed.

## Discovery that actually stays local

`LanDiscoverySocket` sends and receives a fixed 176-byte IPv4 UDP packet. Normal announcements use
multicast group `239.255.69.73`, port `46973`, and an IP multicast TTL of one, so routers do not
forward them by default. Unicast is also exposed for tests and explicitly configured peers.

Each version-1 advertisement carries:

| Field | Meaning |
| --- | --- |
| node | stable 128-bit identity that must be bound to a trusted key outside discovery |
| boot generation | changes on every agent restart and invalidates its outstanding leases |
| sequence | strictly increasing within one boot; stale packets do not extend the TTL |
| TTL | 100–60,000 ms directory lifetime |
| RTT estimate | advisory scheduling input, in microseconds |
| unit slots | maximum and currently active massive units |
| feature bits | classification, refinement, GLSL, identity, and proof capabilities |
| C0–C12 bytes | currently available bytes in each graphics memory coat |
| CRC-32 | corruption detection for the complete packet |

All integer fields use network byte order. The decoder rejects the wrong size, magic, protocol
version, type, payload length, checksum, and structurally invalid capacity.

CRC-32 is not authentication. An advertisement only says that a node might exist.

## Trust boundary

Call `set_trusted(node, true)` only after the host has authenticated that node through an
out-of-band identity or a secure work channel. Production payload and result transport must provide
peer authentication, encryption, replay protection, and message-size limits—for example, an
application-owned mutually authenticated TLS or QUIC session.

Do not deserialize shaders, commands, paths, or executable code from discovery traffic. The
repository deliberately keeps discovery separate from the work carrier so an integrator cannot
mistake multicast reachability for authority.

## Lease contract

`LanAdministrator::lease()` considers only nodes that are:

- unexpired and locally trusted;
- capable of the requested work and extra EIN feature bits;
- within the optional RTT bound;
- below their advertised unit limit; and
- able to reserve every requested C0–C12 byte count.

The deterministic scheduler prefers lower utilization, then lower known RTT, then the lexical node
identity. A lease is capped by both the requested duration and the advertisement TTL. It binds:

- node and boot generation;
- lease, frame, and FULL-seed generations;
- massive-unit coordinates;
- input digest; and
- required feature bits.

The caller sends that self-contained lease and its application payload through the authenticated
work carrier.

## Result gate and fallback

The coordinator must recompute the returned payload's digest and run its local identity, depth,
motion, coverage, and error checks. It places those locally derived values in
`LanVerifiedResult`; raw proof or digest claims copied from the remote node must never be passed to
`submit()`.

A locally verified result is accepted once—and only once—when its node, boot, frame, seed, unit, and
input digest all match; its locally computed output digest is non-zero; every requested
identity/depth/motion proof is present; its unresolved-pixel count is zero; and its maximum error is
finite and within the request bound. This completes the LAN lease but does not itself advance C10
history; the ordinary frame-level proof gate still controls that transition.

A result from the wrong node is ignored without cancelling the genuine lease. A late, restarted,
generation-mismatched, digest-mismatched, incomplete, unresolved, or excessive-error result consumes
the lease and appears in `drain_fallbacks()`. The local coordinator then routes that exact
`LanWorkRequest` to local native work. Duplicate completed results are idempotently rejected.

Call `expire(monotonicMilliseconds)` each scheduler tick. Stale nodes and leases are reclaimed
without waiting for a network disconnect signal.

The discovery socket and administrator do not create background threads. Drive them from the
graphics scheduler, or serialize access when a host uses a separate network thread.

## Minimal coordinator integration

```cpp
ein::LanDiscoverySocket discovery;
discovery.open();

ein::LanAdministrator lan;
if (auto packet = discovery.poll(std::chrono::milliseconds{0});
    packet && packet->advertisement) {
    lan.observe(*packet->advertisement, monotonicMilliseconds());
}

// Only after the secure carrier authenticates the advertised 128-bit identity:
lan.set_trusted(authenticatedNode, true);

ein::LanWorkRequest request = makeMassiveUnitRequest();
if (auto lease = lan.lease(request, monotonicMilliseconds())) {
    secureCarrier.send(*lease, unitPayload);
} else {
    renderUnitLocally(request);
}

for (const auto& fallback : lan.drain_fallbacks())
    renderUnitLocally(fallback.request);
```

The supplied test suite executes codec corruption cases, real UDP loopback discovery, untrusted
node rejection, coat reservation, accepted and duplicate results, proof failures, timeouts, stale
sequences, and boot-generation recovery.
