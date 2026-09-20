# BRONTO checksum article

BRONTO is the release-integrity envelope for EIN Graphics Command Machine:

- **B**ounded: file, manifest, entry-count, expanded-size, and compression-ratio limits are explicit.
- **R**eproducible: canonical JSON fields bind one deterministic release object.
- **O**ffline: validation needs Python 3 and the two downloaded files, not a network service.
- **N**on-recursive: the manifest sits beside the ZIP and never attempts to checksum itself.
- **T**raceable: the envelope records the exact source tag and full commit identity.
- **O**bject integrity: SHA-256 and SHA-512 cover every byte of the release object.

It is deliberately an envelope rather than a new hash algorithm. Inventing a cryptographic hash
would weaken the release. BRONTO composes established hashes, ZIP CRC validation, source identity,
strict parsing, and archive safety into one fail-closed check.

## Separate integrity jobs

The project has three different integrity boundaries. They must not be collapsed into one claim.

| Boundary | Mechanism | Detects | Does not grant |
| --- | --- | --- | --- |
| LAN discovery packet | CRC-32 over the fixed 176-byte packet | accidental transit or storage corruption | peer identity, secrecy, or execution authority |
| LAN work result | locally recomputed 32-byte content digest plus lease/proof fields | payload mismatch against the leased unit | permission to advance history without the ordinary proof gate |
| C12 recovery object | per-chunk and assembled-object SHA-256 | missing, damaged, or mixed recovery shards | live GPU validity, writer identity, or frame proof |
| LUT generation | canonical SHA-256, pinned-key Ed25519, and monotonic switcher | changed values/shape, wrong signer, or generation rollback | upload completion, frame proof, or confidentiality |
| Release archive | detached BRONTO manifest with SHA-256 and SHA-512 | any bit-level archive change, substitution against a trusted pin, unsafe ZIP structure | publisher identity unless the manifest or one of its pins came through a trusted channel |

CRC-32 remains correct for the first job because malformed discovery is discarded and discovery is
never trusted. It would be incorrect for the release job. Conversely, SHA-512 would not make an
unauthenticated discovery sender trustworthy.

## Canonical BRONTO v1 record

The detached JSON document contains exactly ten fields. Unknown and missing fields are rejected so
two implementations cannot silently interpret the same manifest differently.

| Field | Contract |
| --- | --- |
| `format` | Exactly `EIN-BRONTO-1`. |
| `artifact` | One safe `.zip` basename, with no directory component. |
| `artifact_bytes` | Exact byte length of the ZIP. |
| `sha256` | 64 lowercase hexadecimal digits over the complete ZIP. |
| `sha512` | 128 lowercase hexadecimal digits over the complete ZIP. |
| `source_tag` | Bounded release tag, such as `v0.18.0`. |
| `source_commit` | Full 40-digit lowercase Git commit. |
| `zip_root` | Required single top-level directory, ending in `/`. |
| `zip_entries` | Exact central-directory entry count. |
| `zip_uncompressed_bytes` | Exact sum of declared uncompressed member sizes. |

The byte count is checked before hashing. Both hashes are computed in one streaming pass and
compared without data-dependent early exit. Only after the hashes match does the verifier inspect
the archive.

## Bronto-sized archive checks

`scripts/bronto_verify.py` does not extract the ZIP. It applies these checks in order:

1. Reject a symlinked, missing, oversized, non-JSON, non-canonical, or structurally invalid manifest.
2. Check the optional trusted SHA-256 pin and expected tag/commit labels before touching ZIP members.
3. Open one regular-file descriptor and verify byte count, SHA-256, and SHA-512.
4. Bound the entry count, total expanded bytes, and per-entry compression ratio.
5. Reject absolute paths, backslashes, `.`/`..`, empty components, wrong roots, symlinks, special
   files, encryption, non-portable names, Unicode/case-folding collisions, and file/parent conflicts.
6. Decompress every member in memory-sized streams through Python's ZIP reader so each stored CRC
   is checked.
7. Confirm entry and expanded-byte totals and reject an artifact changed during verification.

The fixed bounds are intentionally much larger than this source repository but finite: an 8 GiB
artifact, 100,000 entries, 16 GiB expanded data, and a 1,000:1 per-entry compression ratio. A future
format version can change them explicitly; BRONTO v1 never guesses.

## Produce a manifest

Create the release ZIP from an annotated Git tag, then article it:

```sh
git archive --format=zip --prefix=ein-graphics-command-machine/ \
  --output=../ein-graphics-command-machine-v0.18.0.zip v0.18.0

python3 scripts/bronto_manifest.py \
  ../ein-graphics-command-machine-v0.18.0.zip \
  --output ../ein-graphics-command-machine-v0.18.0.bronto.json \
  --tag v0.18.0 \
  --commit "$(git rev-list -n 1 v0.18.0)" \
  --root ein-graphics-command-machine/
```

Generation validates every ZIP rule and every member CRC before it atomically publishes the
manifest. Existing output is not replaced unless `--force` is explicit.

## Verify offline

When the ZIP and manifest are in the same directory:

```sh
python3 scripts/bronto_verify.py \
  ../ein-graphics-command-machine-v0.18.0.bronto.json
```

For an authenticity anchor, copy the SHA-256 from an independent trusted channel. Tag and commit
expectations are useful release-selection guards, but a label alone does not authenticate ZIP bytes:

```sh
python3 scripts/bronto_verify.py \
  ../ein-graphics-command-machine-v0.18.0.bronto.json \
  --expect-sha256 TRUSTED_64_DIGIT_VALUE \
  --expect-tag v0.18.0 \
  --expect-commit TRUSTED_40_DIGIT_COMMIT
```

A green result means the object is bit-for-bit identical to the manifest, structurally safe under
the declared bounds, and internally CRC-clean. The tag and commit are traceable assertions; compare
or reproduce them from a trusted Git source when provenance matters. Without an independent artifact
hash or signature, an attacker who can replace the ZIP can also replace the manifest and its source
labels. BRONTO states that limit instead of hiding it.

## Failure policy

Any failure is terminal for that downloaded object. Do not extract it, repair it in place, or accept
only one of the two hashes. Fetch the ZIP and manifest again from their intended source. A mismatch
is evidence about bytes, not proof of who changed them.

The adversarial test suite covers a valid object, post-manifest tampering, a forged outer hash with
an inner CRC error, path traversal, case-folding collisions, manifest extension, and trusted-pin
substitution.
