# Why this storeroom works

The shelf has exactly one owner: `main` creates it with `{}` and registers
`defer shelf.deinit();` on the next line. Every exit from `main`, including a
failed `push` passed on by `?`, releases the memory exactly once. The
`inventory` functions never own the shelf; they receive a pointer, borrow
`shelf.items()` for a moment and give it back.

`position` is the single place that knows how to find a name. Because
`add`, `count_of` and `take` all reuse it, fixing a search bug once fixes it
everywhere. It compares text with `std::slice::eq` because two slices with
the same characters usually live at different addresses.

`take` makes every decision before it changes anything. That is what keeps a
refused order harmless: either the whole operation happens or none of it.
The pointer `Item* item = &shelf.items()[index]` is used right away and never
kept, because `remove` (like `push`) may move the elements.

`main` matches on the `Status` that `take` returns. The nested `match` on
`StockError` has to cover both variants, so if someone adds a third kind of
refusal later, the compiler points at this report.
