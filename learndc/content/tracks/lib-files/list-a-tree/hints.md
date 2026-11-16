## Hint 1

The function does the same thing at every level: list one folder, and for a
folder entry do it again one level deeper. Take care of the output of the
current level before you recurse.

## Hint 2

Get the label with `name.text[0..name.len] as []const char`. For the
child path: `char[256] storage = {};`
`[]const char child = std::os::path::join(folder, label, storage[0..256])?;`.
Compare `name.kind == EntryKind::Directory` to tell folders from files.

## Hint 3

```dc
for u32 i = 0; i < depth; i++ {
    print("  ")?;
}
if name.kind == EntryKind::Directory {
    println("{}/", label)?;
    totals.folders += 1;
    list_tree(child, depth + 1, totals)?;
} else {
    u64 size = std::os::file::metadata(child)?.size;
    println("{} ({} bytes)", label, size)?;
    totals.files += 1;
    totals.bytes += size;
}
```
