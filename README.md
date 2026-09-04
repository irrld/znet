# znet

znet is a modern C++ networking library for applications that send structured
messages: define a packet, register a serializer, send it. Encryption,
compression and framing are handled underneath. It is designed to be simpler and
more approachable than low-level libraries like asio or libuv, which give you a
socket and an event loop where znet gives you sessions, typed messages and an
event callback.

## Features

- ✅ **Simple API**: clean, event-driven design.
- 📦 **Built-in packet serialization**: define your own packets easily.
- 🔒 **Encryption and compression**: AES-256-GCM and zstd, negotiated during the
  handshake. Read
  [what the crypto does and does not give you](https://github.com/teoncreative/znet/wiki/Encryption-and-Compression)
  before relying on it; it is not TLS and does not authenticate the peer.
- ⚡ **Async connect**: non-blocking connections.
- 🛠 **Cross-platform**: Windows, Linux, macOS.

## Installation

Requires **C++14 or newer** (C++20 by default), **CMake 3.29+** and **OpenSSL**,
which is found automatically if installed. zstd is bundled.

👉 **[Getting Started](https://github.com/teoncreative/znet/wiki/Getting-Started)**
has the submodule and CMake steps, using either the bundled zstd or your own,
plus every build option.

## What it looks like

**Server:**

```cpp
ServerConfig config{"127.0.0.1", 25000};
Server server{config};
server.SetEventCallback(...);
server.Bind();
server.Listen();  // async listen
```

**Client:**

```cpp
ClientConfig config{"127.0.0.1", 25000};
Client client{config};
client.SetEventCallback(...);
client.Bind();
client.Connect();  // async connect
```

Neither config names a transport, so both get the default, ZDT. See the
[examples](examples) folder for full working code, and the wiki below for
everything else.

## Documentation

👉 **[Read the Wiki](https://github.com/teoncreative/znet/wiki)**

* [Getting Started](https://github.com/teoncreative/znet/wiki/Getting-Started):
  build it, then a server and client that talk
* [Packets and Serialization](https://github.com/teoncreative/znet/wiki/Packets-and-Serialization):
  defining messages
* [Events](https://github.com/teoncreative/znet/wiki/Events): every event and
  what to do in each
* [Session State](https://github.com/teoncreative/znet/wiki/Session-State):
  attaching your own per-connection object to a session
* [Choosing a Transport](https://github.com/teoncreative/znet/wiki/Choosing-a-Transport):
  ZDT or TCP, and ZDT's per-message delivery modes
* [Threading Model](https://github.com/teoncreative/znet/wiki/Threading-Model):
  which thread calls your code
* [Configuration Reference](https://github.com/teoncreative/znet/wiki/Configuration-Reference):
  every option, including metrics and the per-transport groups
* [Encryption and Compression](https://github.com/teoncreative/znet/wiki/Encryption-and-Compression):
  what the crypto does and does not give you
* [Metrics](https://github.com/teoncreative/znet/wiki/Metrics): the counters, and
  reading them honestly
* [Extensions](https://github.com/teoncreative/znet/wiki/Extensions): optional
  add-ons for smaller packets, automatic serialization and engine types
* [Peer-to-Peer](https://github.com/teoncreative/znet/wiki/Peer-to-Peer):
  gathering, rendezvous, hole punching and the relay fallback
* [API Stability](https://github.com/teoncreative/znet/wiki/API-Stability):
  which parts of the API may change, and how much notice you get

## Benchmarks

znet against three peer libraries and a raw-socket floor, on clean loopback and
over a link with 5% packet loss and a 50 ms round trip. Throughput is messages
per second at three payload sizes; latency is a 64 B ping-pong. `znet` is the
default build (AES-256-GCM plus zstd); `znet-raw` turns both off, the
like-for-like row against ENet and RakNet, which send plaintext. Every row is
the median of five runs on one machine (Ryzen 9 9950X3D, Linux 7.2, GCC 16), so
they compare with each other and nothing else.

[benchmarks/README.md](benchmarks/README.md) is how to reproduce them and how
to read them: what each measurement does and does not support, why the floors
are floors, and the open problems.

### Over a lossy link

Message counts are scaled for the longer round trip, so these compare only with
each other. Latency percentiles are milliseconds.

|                        |       64 B |     1 KiB |         8 KiB |     p50 |   p95 |   p99 |
|------------------------|-----------:|----------:|--------------:|--------:|------:|------:|
| **znet ZDT**           | **27,471** | **8,959** |     **1,499** |    50.1 |   151 |   176 |
| **znet ZDT-raw**       |     26,285 |     8,815 |         1,500 |    50.0 |   151 |   176 |
| GNS (Valve) ₁          |     22,629 |       823 |            35 |    50.1 |   260 |   262 |
| ENet                   |     16,356 |     1,066 |           131 |    50.0 |   112 |   112 |
| raw UDP *(floor)* ₂    |     38,364 |    38,899 |         5,093 |    50.0 |  50.0 |  50.0 |
| raw TCP *(floor)*      |      2,946 |       136 |            18 |    50.1 |   404 |   469 |
| znet TCP               |      1,282 |       134 | *unsupported* |    50.0 |   308 |   431 |
| RakNet                 |      1,032 |    69 ₃   |         9 ₃   |    70.1 |   230 |   311 |

znet ZDT leads every reliable transport at every size: against GNS that is 1.2x,
11x and 43x, against ENet 1.7x, 8.4x and 11x. Encryption costs nothing here, the
default and raw rows are inside each other's spread, because a lossy link is
never CPU-bound. The raw UDP row is unreliable and drops what it loses, so it is
a datagram-rate ceiling rather than a rival.

### Clean loopback

|                        |          64 B |       1 KiB |         8 KiB | encryption  |
|------------------------|--------------:|------------:|--------------:|-------------|
| ENet                   | **5,505,973** | **1,137,134** |   **162,442** | none        |
| **znet ZDT-raw**       | **2,162,583** | **460,909** |    **94,645** | none        |
| **znet ZDT**           | **1,968,692** | **396,244** |    **73,647** | AES-256-GCM |
| raw UDP *(floor)*      |     1,495,737 |   1,449,640 |       986,895 | n/a         |
| GNS (Valve) ₁          |     1,481,565 |      98,792 |        11,675 | AES-GCM     |
| raw TCP *(floor)*      |       546,213 |     527,331 |       467,391 | n/a         |
| RakNet                 |        48,289 |      41,567 |        74,969 | none        |
| znet TCP-raw           |     1,013,401 |     737,535 | *unsupported* | none        |
| znet TCP               |     1,002,566 |     365,212 | *unsupported* | AES-256-GCM |

Latency, a 64 B round trip in microseconds:

|                  |      p50 |      p95 |      p99 |
|------------------|---------:|---------:|---------:|
| raw UDP *(floor)*|      3.1 |      4.4 |      6.4 |
| ENet             |      3.6 |      3.6 |      3.6 |
| raw TCP *(floor)*|      6.5 |      7.3 |      9.6 |
| GNS (Valve)      |     12.5 |    1,068 |    2,121 |
| **znet ZDT-raw** | **13.0** | **16.1** | **21.4** |
| **znet ZDT**     | **13.9** | **17.9** | **22.8** |
| znet TCP         |    252.4 |    255.3 |    268.6 |
| RakNet           |   20,074 |   20,142 |   20,168 |

ENet leads on a clean link; of the encrypted datagram transports znet ZDT holds
its tail within 1.6x of its median where GNS runs to 170x its own. The default
profile costs about 1.1x against raw at 64 B and 1 KiB and is inside the spread
at 8 KiB, so it is per-message overhead, not per-byte. Loopback runs neither
congestion control nor loss recovery, so it flatters TCP; see
[why](benchmarks/README.md#why-the-clean-tables-flatter-tcp).

### Under sustained load

A 1 KiB transfer runs for ten seconds with a 64 B probe travelling through it.
`steady` is the rate over the second half, past slow-start; `loaded` is what the
probe costs while the link is saturated. The two compare like for like only
within one `probe` value: `channel` is the probe on its own ordered stream,
`conn` on its own connection, `none` on the same stream as the transfer.

|                        | probe   |  clean steady | clean loaded p50 | lossy steady | lossy loaded p50 |
|------------------------|---------|--------------:|-----------------:|-------------:|-----------------:|
| ENet                   | channel | **1,110,320** |          3.8 ms  |        1,041 |         121.6 ms |
| raw TCP *(floor)*      | conn    |       521,153 |     **0.01 ms**  |          162 |          50.1 ms |
| **znet ZDT-raw**       | channel |   **407,357** |          0.02 ms |    **9,726** |          80.1 ms |
| **znet ZDT**           | channel |   **376,974** |          0.03 ms |    **9,738** |          80.1 ms |
| GNS (Valve)            | none    |        87,201 |         47.5 ms  |        1,331 |         338.9 ms |
| RakNet                 | channel |        47,607 |         87.5 ms  |           65 |             ₄    |
| znet TCP               | none    |       365,144 |         11.0 ms  |          127 |             ₄    |

Under loss ZDT carries several times what GNS and ENet do while holding the
probe below either, which is the trade a delay-sensitive controller exists to
make. On a clean link its channel keeps the probe at a hundredth of a
millisecond, next to the raw TCP floor.

### System calls per message

Counted at the libc boundary on the clean run, so a message's cost in kernel
entries is comparable across libraries. Small messages batch into a datagram, a
1 KiB message is about one datagram each way, and 8 KiB fragments across the
MTU, which is what the three columns track.

|                  |   64 B |  1 KiB |  8 KiB |
|------------------|-------:|-------:|-------:|
| RakNet           |   0.11 |   2.00 |  14.00 |
| ENet             |   0.17 |   2.11 |  14.87 |
| **znet ZDT-raw** |   0.21 |   2.71 |  14.61 |
| **znet ZDT**     |   0.27 |   2.62 |  15.59 |
| GNS (Valve)      |   0.59 |   1.96 |  16.22 |

The four are within a call of each other for the same datagram count. ENet under
loss is the exception and is left out: it is serviced in a tight application
loop, so a rare delivery charges the whole spin to that message rather than to
the protocol.

### Reading the numbers

A single run under loss is not a number: several throughput cells vary by more
than a quarter across five runs, so read every row as a median with its spread.
The footnotes and the per-row caveats are in
[benchmarks/README.md](benchmarks/README.md).

₁ GNS clamps its own send rate internally, so its 1 KiB and 8 KiB rows measure
that limiter rather than the protocol.
₂ Unreliable: at 5% loss it drops what it loses, so its count is what arrived,
not what a reliable protocol had to recover.
₃ Did not finish inside the 60 s deadline; the row reports the truncated rate.
₄ Too few probes returned inside the timeout to quote a percentile, which is
itself the result: the link was too congested for a small message to complete.

## Contributions

We welcome and encourage community contributions to improve znet. If you find any
bugs, have feature requests, or want to contribute in any other way, feel free to
open an issue or submit a pull request.

## License

Apache License 2.0. See [LICENSE](LICENSE) for details.
