## Hint 1

Both fingerprints end the same way: `std::sha::hex_encode(digest[0..32], out[0..64]);` and `return out[0..64] as []const char;` (a `[]char` converts to `[]const char`).

## Hint 2

One-shot: `std::sha::sha256(text as []const u8, digest[0..32]);`. Incremental: create `std::sha::Sha256 hasher = std::sha::Sha256::new();`, call `hasher.update(first as []const u8);` and the same for `second`, then `hasher.finish(digest[0..32]);`.

## Hint 3

`same_content`: fill two `u8[32]` digests and return `std::mem::equal(da[0..32] as []const u8, db[0..32] as []const u8)`. `shard_of`: `return std::hash::fnv1a_32(name as []const u8) % shards;`.
