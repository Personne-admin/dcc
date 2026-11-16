## Hint 1

Both functions mirror each other: one side `accept`s, the other `connect`s; each writes a line and reads a line. The newline is what tells `read_line` the message is complete.

## Hint 2

`serve_connection`: `Result(TcpStream, std::os::error::Error) accepted = server.listener.accept();`, return `false` when `accepted.is_err()`, otherwise `peer = accepted.unwrap()` and `defer peer.close();`. Read with `read_line(&peer, line[0..255], &length)`.

## Hint 3

Reply: `uppercase(line[0..length]); line[length] = '\n' as u8; return peer.write_all(line[0..length + 1]).is_ok();`. Client: copy `text` into a `u8[256]` message, add the newline, `stream.write_all(message[0..text.len + 1])?;`, then `read_line(&stream, reply, &length)`.
