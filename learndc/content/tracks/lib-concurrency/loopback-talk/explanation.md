# Why a server thread and a line protocol

Two blocking calls face each other: the server waits in `accept` for a client,
and the client waits in `read_line` for the reply. Neither can proceed on a
single thread of control unless the order is arranged very carefully, so the
server gets its own thread and the two sides simply block until the other is
ready. The thread is joined before the counter is read, so the count is complete.

TCP delivers a stream of bytes with no boundaries, so every protocol needs a
rule for where a message ends. A newline is the simplest: `read_line` keeps
reading until it sees one, and returns `false` if the connection ends first
or the line does not fit. A length prefix is the other common choice; the
newline needs no counting but cannot carry a newline inside the message.

Loopback with an OS-chosen port is what makes this a *reliable* program: no
fixed port to collide with, no other machine to be slow or absent, and traffic
that never leaves the kernel. Replies arrive in the order the requests were
made because the client does one conversation at a time and the server
handles connections one after another: the order is a property of the
design, not of luck.
