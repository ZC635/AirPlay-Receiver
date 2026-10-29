# QMdnsEngine provenance and local changes

Baseline: upstream **0.2.1**, downloaded from
https://github.com/nitroshare/qmdnsengine/archive/refs/tags/0.2.1.zip.
The original MIT license and source copyright notices are retained.

Original Git blob identifiers before modification:

- `src/src/server.cpp`: `e91566e364b77548b18c9de5f50d08996b4c35b8`
- `src/src/server_p.h`: `cf0577db5701e03495422e19458031d9c4d62a69`
- `src/src/hostname.cpp`: `94d9931a8d9154300274ca6e0d0d1991ba103377`
- `src/src/message.cpp`: `9fe2a9f42efdf0a989140f1ca74b483354159833`

AirPlay Issue #11 changes:

- `Message` carries a local interface index and generation through copy/reply.
  DNS encoding remains unchanged. Old DLLs are not compatible with the added API.
- `Server` reads interface metadata, drains pending datagrams, sends replies on
  the receiving link and sends probes/announcements on each joined link. Source
  addresses, IPv6 scope and hop limit are explicit. Stale replies are discarded.
- Memberships are reconciled every two seconds, and once more when a datagram
  arrives from a link that is not yet joined. Only up/running, multicast-capable,
  non-loopback interfaces with usable addresses participate. A failed join or
  bind is reported once through `transportEvent` and retried silently; it is not
  a server `error`. Interface/address changes and membership recovery emit
  `AbstractServer::interfacesChanged` after reconciliation. Each join carries
  its own generation, so a reply composed before one link changed is dropped
  without affecting replies on other links.
- `Hostname` and `Provider` listen to `interfacesChanged`: the hostname withdraws
  only address records that are no longer true on a link, announces new ones,
  and leaves unchanged records alone; providers repeat their announcement.
  Nothing is recreated or reprobed. Address replies come from the records
  announced on the receiving interface, so they remain available during the
  periodic reprobe and need no interface enumeration per query.
- Host records use cache-flush and a 120-second TTL, are announced on
  registration, and are withdrawn using saved records on replacement/stop.
  Zero-TTL goodbyes are not treated as hostname conflicts during reprobe.
- Legacy unicast replies repeat the question/ID and cap TTL at 10 seconds without
  cache-flush (RFC 6762 section 6.7), preserving compatibility when host-record
  cache-flush is enabled for multicast clients.
- Windows IPv4 uses socket-level IP_TTL and IP_MULTICAST_TTL. A native Qt 6.11
  datagram matrix reproduced WSAEINVAL (10022) whenever IPv4 per-packet hop limit
  was provided, independently of interface/source selection; IPv6 accepts it.
  The simulated Windows boundary rejects that same IPv4 combination.
- `transportEvent` distinguishes accepted UDP writes from errors. No delivery
  acknowledgement is implied. The application forwards these events to its
  existing opt-in diagnostic sink.
- The parent `CMakeLists.txt` builds a project-owned shared library with Qt 6;
  upstream packaging/example configuration is not used. The application ships
  this build's DLL, never the MSYS2 binary.

Application integration only forwards `interfacesChanged` through its
diagnostic adapter; the library re-announces on the affected links without
restarting the AirPlay backend.

See `tests/mdns/README.md` for verification and remaining limitations.
