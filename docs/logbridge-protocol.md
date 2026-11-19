# Log bridge protocol

The log bridge is how `ZeroIV.asi`, running inside the game process, delivers
its log to ZeroIV's debug console in real time.

This document describes the wire format only. It exists so the two sides can be
read, audited and reimplemented independently: the loader is GPL-3.0-or-later,
the ZeroIV application is not, and neither is a derivative of the other. They
are two separate programs that exchange bytes over a socket — see
[Licensing boundary](#licensing-boundary).

- Loader side: `src/utils/logbridge.cpp` in this repository.
- Application side: `logbridge.go` in the ZeroIV application.

---

## 1. Transport

| Property | Value |
| --- | --- |
| Protocol | TCP over loopback |
| Direction | the game process **dials**; the application **listens** |
| Default address | `127.0.0.1:27183` |
| Encoding | UTF-8, one record per line, `\n` terminated |
| Connection lifetime | one game session (the process keeps it open) |

The game is the client. This matters: ZeroIV may start before or after the
game, and a restart of either side must not require restarting the other. The
client connects lazily on the first record it wants to send and reconnects on
its own when the socket drops.

The port is configurable on both sides and the two settings have to agree:

```ini
; ZeroIV.ini, read by the loader at startup
[debugConsole]
enabled = true
host = 127.0.0.1
port = 27183
```

The application's listener port is a build-time constant
(`loaderLogBridgePort`). A user who changes the ini to a different port without
changing the application gets a silently dead console, so the loader logs the
address it dials when `[log] info` is on.

Both ends bind/dial an **IP literal**, never a hostname: the listener rejects
any peer whose address is not a loopback address, and a hostname would add a
DNS lookup to a path that must never block the game thread.

---

## 2. Framing

A connection carries two kinds of line:

1. **Exactly one greeting**, as the very first line.
2. **Zero or more records**, one per line.

Both are `\n` terminated. A line may be up to 16 KiB in the application's
reader; anything longer is truncated by the reader before parsing (the loader
already caps messages at 2000 bytes, see §5).

### 2.1 Greeting

```
ZEROIV-LOADER/1 ZeroIV.asi <version>\n
```

- `ZEROIV-LOADER/1` — protocol identifier and major version. The reader accepts
  the connection as "greeted" only when the first line starts with this exact
  token. Any future incompatible change bumps the number.
- `ZeroIV.asi` — the component that dialed. Reserved for future bridges that
  may share the port; today it is always this literal.
- `<version>` — the loader's version string, e.g. `1.0.0+7faccf43`. It is
  informational and is surfaced in the console as
  `ZeroIV-Loader connected (v1.0.0+7faccf43)`.

The greeting is optional in the sense that a missing or malformed one does not
abort the connection: the reader simply never enters the "greeted" state, and
record lines are still parsed. This keeps a future sender without a greeting
usable, and makes the failure mode of an old sender a missing banner rather
than a dead console.

### 2.2 Record

```
<level>|<module>|<unixMillis>|<message>\n
```

| Field | Format | Notes |
| --- | --- | --- |
| `level` | `info`, `warn` or `error` | Writer's choice; empty means `info`. The reader does not validate it, so a future level passes through. |
| `module` | short token, no `\|` | The loader's log category (`loader`, `device`, `mods`, `pack`, `rpf`, …). Empty means `zeroiv-loader`. |
| `unixMillis` | decimal integer | Milliseconds since the Unix epoch, UTC. Invalid or `<= 0` is replaced with the reader's current time. |
| `message` | free text | May contain `\|`; only the first three separators are significant. |

Splitting rule: split on `|` with a **limit of 4**, so everything after the
third separator is the message. `strings.SplitN(line, "|", 4)` on the reading
side, and a plain concatenation on the writing side.

Example:

```
info|pack|1732000000123|dlcpacks:/nisa31/dlc.rpf is unpacked: mounting ZeroIVLoose/dlc_rpf_af9b654b.rpf
```

Timestamps come from the game process's clock. The reader keeps them instead of
stamping on arrival, because the loader queues lines while ZeroIV is closed and
delivers the backlog later; arrival time would be wrong for all of those.

---

## 3. Connection lifecycle

### Client (game)

1. Records are appended to an in-memory queue of at most **1024 lines**. When
   the queue is full the oldest line is dropped, so a long session with no
   application running cannot grow without bound.
2. A detached sender thread owns the socket. It connects on demand, sends the
   greeting, then drains the queue.
3. If connecting fails, the thread sleeps **2000 ms** and retries. The line it
   was holding is pushed back to the front of the queue.
4. If a write fails, the socket is closed and the same put-back-and-retry rule
   applies. The next successful connection starts with a fresh greeting.
5. When the queue is empty the thread waits up to **3000 ms** on a condition
   variable. This idle wake-up is what makes a late-started ZeroIV pick up the
   backlog without any new log activity.

Nothing on this path may block the game thread: `enqueue` only takes a mutex
and notifies, and it swallows every exception.

### Server (application)

1. Accepts a connection, rejects it unless the peer address is a loopback
   address.
2. Reads lines until EOF or error. There is no read deadline: an idle
   connection is normal (a quiet game produces no lines).
3. Records are handed to a batching goroutine and pushed to the console as
   **one event per 120 ms or per 128 lines**, whichever comes first. This keeps
   a chatty loader from generating one WebView IPC event per line.

### Reconnect semantics

Either side may close at any time and the other must cope:

| Event | Loader behaviour | Application behaviour |
| --- | --- | --- |
| ZeroIV closed | connect fails, retry every 2 s | — |
| ZeroIV restarted | next connect succeeds, greeting repeated | new connection is a new session |
| Game closed | — | read returns EOF, connection dropped |
| Port already in use | connect fails, retry every 2 s | listener fails to bind; the console still shows in-app lines |

Lines emitted while disconnected are not lost (they stay queued, bounded by
1024), but lines dropped from a full queue are gone: the bridge is diagnostics,
not an audit log.

---

## 4. Flood control

Two independent limiters, because the loader's own throttling must not be the
only defence:

**Writer.** Each log category is opt-in via `[log]` in `ZeroIV.ini`, repeated
identical lines are collapsed (one line per 1 s window, then a
`previous line repeated Nx` summary), and a category is rate-limited before it
ever reaches the queue: 120 lines/s for the chatty categories (`device`,
`mods`, `rpf`) and 500 lines/s for the rest. Over-budget lines are dropped and
summarised as `dropped N line(s) from '<category>'`. Lines written with
`logger::write` (startup, errors) bypass the rate limit but still collapse
repeats.

**Reader.** Per connection, at most **400 records per second**. When a one
second window closes having dropped lines, the reader emits one synthetic
record:

```
warn|logbridge|<now>|suppressed N loader line(s): flood limit reached
```

so truncation is visible in the console instead of silent. Records that do not
fit the batch channel are counted as dropped by the same counter.

A record longer than **16 KiB** is truncated by the reader and marked with `…`.

---

## 5. Text hygiene

The loader sanitizes each message before queueing:

- `\r` and `\n` are replaced with a single space, so a message can never forge
  a second record.
- The result is capped at **2000 bytes**.

The reader applies its own limits (16 KiB line, see §4) and treats every line as
data: it is never evaluated, never used to build a path, and never written to a
shell. The console renders it as text.

This is the reason the message field is last and unbounded in practice: multi
line diagnostics get folded into one record rather than being split.

---

## 6. Failure modes and what they look like

| Symptom | Likely cause | Where to look |
| --- | --- | --- |
| Console shows nothing from the game | port mismatch, `[debugConsole] enabled=false`, or the ASI never loaded | `ZeroIV.log` next to the ASI; application log `[logbridge] listening on …` |
| Banner appears but no records | `[log]` categories all off | the startup line lists the resolved roots; per-category switches are in `[log]` |
| Records stop mid-session | game closed, or queue overflow reported as `suppressed N` | console `logbridge` module |
| `ZeroIV-Loader connected` twice | the game was restarted, or the socket was reset | expected; each connection is a session |
| Console freezes while loading | should not happen: batching and the per-second cap exist for this | application log |

The bridge never carries anything that is not a log line. It is one-way, has no
authentication, and is only reachable from the same machine.

---

## 7. Versioning

- The identifier carries a **major** number (`ZEROIV-LOADER/1`).
- Adding a field, changing a level name or altering the greeting's tail is
  compatible and does not bump it.
- Changing the separator, the field order, or the framing bumps it. A reader
  that does not recognise an identifier should keep parsing records (they are
  self-describing enough to fail gracefully) and skip the greeting.

---

## 8. Licensing boundary

This protocol is an interface, not shared code.

- `ZeroIV.asi` is GPL-3.0-or-later. Its source, including the sender
  implementation, is published with every release.
- The ZeroIV application is a separate program with its own licence. It does
  not link against, embed, or contain any part of the loader.
- The two communicate only by these bytes over a TCP socket, which is
  inter-process communication between independent works, not a derivative
  work of either.

Neither side is required to publish the other's code, and neither side's
licence extends to the other. Any future bridge for another component should
reuse this framing under a new `<component>` token in the greeting rather than
forking the format.
