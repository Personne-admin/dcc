# Why a hook writes to a writer

A hook does not return a string, because a string needs memory and a place to
live. It writes to whatever `W` the caller chose. The same `Money` hook
therefore prints to the terminal, fills a fixed buffer or only counts the
bytes it would write, with no allocation of its own and with the caller's
error type. That is the reason for the odd-looking return type: errors from
the writer come back unchanged, as `FormatError::Write(e)`.

`Level` honours the placeholder because it writes a single word: padding a
word is well defined, and `write_padded` is the library's own padding
routine, shared with the built-in text and integers. `Money` writes several
pieces; padding them as one unit would need the finished text first, which
means a buffer. Ignoring `opts` there is a design choice, not an oversight.

The tiny buffer in `main` ends the lesson with the property that makes writers
safe: formatting into too little room is an ordinary error value
(`is_ok()` is false), never a write past the end of the array.
