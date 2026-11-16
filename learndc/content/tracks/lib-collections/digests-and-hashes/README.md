# Fingerprints and shards

A **hash function** turns any amount of data into a fixed-size number. The
library has two families, for two different jobs.

| | `std::sha` (SHA-1, SHA-224/256, SHA-384/512) | `std::hash::fnv1a_32` / `fnv1a_64` |
| --- | --- | --- |
| Output | 20 to 64 bytes | one `u32` or `u64` |
| Speed | slower | very fast |
| Collisions | practically impossible to find on purpose | easy to build on purpose |
| Use for | fingerprints, integrity checks, "is this the same content?" | hash tables, spreading names over shards or buckets |

Never use FNV where an attacker or a mistake must not be able to produce two
inputs with the same hash, and never use SHA-256 where a tight loop needs
millions of hashes per second. (SHA-1 is kept for old formats only.)

## SHA-256

```dc
import std::sha;

u8[32] digest = {};
std::sha::sha256(bytes, digest[0..32]);          // one call: bytes in, digest out

std::sha::Sha256 hasher = std::sha::Sha256::new();
hasher.update(part_one);                         // as often as you like
hasher.update(part_two);
hasher.finish(digest[0..32]);                    // same digest as hashing both at once

char[64] hex = {};
std::sha::hex_encode(digest[0..32], hex[0..64]); // 64 lowercase hex characters
```

- Text is `[]const char`; the digest functions take `[]const u8`, so convert
  with `text as []const u8`.
- `sha256` and `finish` return a `Status(ShaError)`; the only error is an
  output slice that is too small, which these fixed sizes rule out. The
  example code does not need to check it.
- The incremental form exists for data that arrives in pieces (a file read
  in blocks): the digest is the same however the bytes are split.
- Digests compare as bytes: `std::mem::equal(a, b)` on two `[]const u8`.

## FNV-1a

`std::hash::fnv1a_32(bytes)` is a 32-bit hash with a fixed, published
definition, so it returns the same number on every machine and every run.
`hash % shards` assigns the input to one of `shards` buckets.

## Your task

Implement four functions in `src/main.dc`; `main` is finished.

1. `fingerprint(text, out)` writes the SHA-256 hex of `text` into `out` and
   returns `out[0..64]`.
2. `fingerprint_parts(first, second, out)` does the same for the two texts
   joined, but with a `Sha256` value and two `update` calls.
3. `same_content(a, b)` is `true` when both texts have the same digest.
4. `shard_of(name, shards)` is the FNV-1a 32-bit hash of the name's bytes
   modulo `shards`.

Expected output:

```text
empty: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
abc:   ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
parts: b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
whole: b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
same: true
same: false
   alpha -> shard 3
    beta -> shard 3
   gamma -> shard 2
   delta -> shard 1
 epsilon -> shard 1
    zeta -> shard 3
```

## Acceptance criteria

- The project builds with `dpm build`.
- Output matches exactly.
