# Two hashes, two jobs

The digest of `hello world` is the same whether the text arrives in one
piece or two: `update` only buffers and mixes bytes, and `finish` appends the
length and padding once. That property is what lets a program fingerprint a
file it reads block by block without holding the whole file.

`same_content` compares 32 bytes instead of the texts. For two documents in
memory that is no gain, but a program that stores digests of thousands of
files can recognise a duplicate without keeping the files. Equal digests mean
equal content for SHA-256 for all practical purposes; that guarantee is
exactly what FNV does not give. With only 2^32 values, two different names
can share a shard (alpha and beta both land in shard 3 above), which is
fine for balancing work and unacceptable for identity.

The fixed definition of FNV-1a is why `shard_of` can be tested with exact
expected output. The hash that `std::map` uses internally is allowed to
change between library versions; a shard number that is stored on disk or
sent to another machine must come from an algorithm that never changes.
