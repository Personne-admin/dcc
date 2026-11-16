# A conversation over loopback

`std::os::net` has TCP sockets. A **server** binds an address and *accepts*
connections; a **client** *connects* to that address. Both ends then read and
write bytes on a `TcpStream`. This lesson keeps everything on one machine and
one program, on the **loopback** address `127.0.0.1` (`net::IPV4_LOOPBACK`),
where no network is involved and nothing outside the program can interfere.

```dc
any.ip = std::os::net::IPV4_LOOPBACK;
any.port = 0;                                        // 0: the system picks a free port
listener = std::os::net::bind(any)?;
address = listener.local_addr()?;                    // learn which port it chose

std::os::net::TcpStream peer = listener.accept()?;   // server side: waits for a client
std::os::net::TcpStream stream = std::os::net::connect(address)?;   // client side
stream.write_all(bytes)?;                            // send everything
usize got = stream.read_all(buffer)?;                // fill buffer, or stop at the end of the data
stream.close();                                      // every stream and listener needs a close
```

- A stream carries **bytes**, not messages. The two sides must agree where a
  message ends. Here a message is one line: its text followed by `'\n'`.
  `read_line`, which reads bytes one at a time until the newline, is provided.
- `accept()` blocks until a client connects, and `read_all` blocks until its
  buffer is full or the peer closes. That is why the server runs on **its own
  thread** (`serve`): while it waits in `accept`, `main` is free to connect.
- Choosing port 0 avoids clashes with whatever else runs on the machine.
- Close what you open: `defer peer.close();` for the accepted stream,
  `defer stream.close();` for the client's, and `main` closes the listener.
  Both sides closing is also how the peer notices that the conversation is over.
- The thread is joined before `main` reads the server's `handled` counter.

## Your task

`serve` (the thread function), `read_line` and `main` are finished. The server
handles three connections one after another and counts the ones that went well.
Implement:

1. `serve_connection(server)`: accept one client from `server.listener`, read
   its line with `read_line`, uppercase the line (`uppercase` is provided),
   append a newline and write it back. Return `true` if everything worked.
   `defer` closing the accepted stream.
2. `ask(address, text, reply)`: connect to the address, send `text` and a
   newline, read the server's line into `reply` with `read_line`, and return
   its length. Close the stream with `defer`.

Expected output:

```text
sent  5 bytes, got back: [HELLO]
sent 14 bytes, got back: [DC ON LOOPBACK]
sent  0 bytes, got back: []
server handled 3 connections
```

## Acceptance criteria

- The project builds with `dpm build`.
- Output matches exactly, every run.
- No stream is left open.
