# Multi-interface discovery (Issue #11)

The receiver advertises on every eligible interface. It does not prefer Ethernet,
Wi-Fi, a default route, or a named adapter. Virtual adapters use the same eligibility
rules. This provides discovery on each attached link; it does not forward mDNS
between networks or bypass AP isolation and firewall rules.

## Transport and lifecycle

The bundled QMdnsEngine 0.2.1 patch retains the receiving interface through DNS
reply construction. Replies use that interface's source address and outlet.
Announcements, probes and goodbyes are sent on all joined interfaces. A/AAAA
answers contain only addresses assigned to the receiving interface.
They carry cache-flush and are announced proactively. Address replacement and
stop withdraw the previously announced addresses, so existing clients need not
wait for an old cached address to expire. The address-change case checks both
old-address withdrawal and new-address cache replacement.

Missing interface metadata is accepted only when one eligible joined interface
exists for the packet's address family. Ambiguous packets are diagnosed and
dropped rather than guessed from the default route or overlapping subnets.

Interfaces and addresses are checked every two seconds. Changes reconcile
multicast membership, invalidate old reply contexts, and queue a hostname/provider
restart. The normal probing timers run before fresh announcements. Existing
AirPlay transport sessions are not restarted. Membership failures are retried;
one failing interface does not prevent service on another. Rapid changes wholly
between polls are not guaranteed to be observed.

## Automated regression suite

`tests/mdns` compiles the production publisher, adapter and vendored DNS code.
Only interface enumeration and UDP transport are replaced by an in-memory network.
Queries and replies pass through real DNS encoding/decoding and registration.
The default outlet is a controlled input, not a prediction of Windows routing.

With the MSYS2 UCRT64 tools on PATH:

```powershell
cmake -S tests/mdns -B .tmp/mdns-tests -G Ninja
cmake --build .tmp/mdns-tests
ctest --test-dir .tmp/mdns-tests --output-on-failure
```

The suite is also included in the normal `AIRPLAY_WITH_UXPLAY=ON` CTest build.
On Windows it also models Winsock rejecting per-datagram IPv4 TTL. Production
uses socket-level IPv4 TTL while retaining explicit packet source and interface;
IPv6 retains its per-datagram hop limit. Both paths require an effective TTL of 255.
It covers both services, both default-outlet choices, IPv6 and scope, overlapping
subnets, legacy unicast replies, interface metadata loss, ineligible adapters,
hotplug, address replacement, stale replies, failed membership and failed sends.
Stopping publication must withdraw records and stop future network activity.
Address replacement checks both withdrawal of the old address and cache-flush
on the new address. Legacy unicast replies must repeat the question/ID, omit
cache-flush and cap TTL at 10 seconds, per RFC 6762 section 6.7.

Before the patch, the single-LAN control passed and five initial discovery tests
failed. The original lab is not a release test: its passing cases included
expected failures. The committed suite requires successful discovery.

`MdnsRuntimeDeploymentTest` checks that the deployment helper replaces stale DLLs
with the build output and fails if that output is absent. The build script excludes
the MSYS2 DLL from bulk copying and verifies the final library's SHA256.

## Diagnostic interpretation and real-device acceptance

- `search_received`: query reached the application; `interface` is its ingress.
- `send_requested`: protocol code requested a reply; no successful write implied.
- `send_result result=accepted`: UDP write accepted, with `interface`, address
  family, scope and byte count. This is not proof of reception by an iPhone.
- `multicast_join` / `multicast_leave`: membership operations and their results.
- `interfaces_reconfigured`: the service publishers are being reprobed.
- `receive_dropped` / `send_dropped`: missing, inactive or stale interface context.

Use the existing diagnostic restart option when collecting evidence. For a real
Windows dual-interface acceptance run, keep both interfaces connected, query from
each network, and capture UDP 5353 on both PC interfaces. Check the reply outlet,
source and A/AAAA records; reverse default-route priority and repeat. Then verify
iPhone discovery, connection and first video frame, including after reconnecting
an interface. Socket write success alone cannot rule out a firewall or router
problem. Simulated discovery does not verify Windows routing or video streaming.
`DnssdRuntimeAcceptanceTest` additionally checks real Windows UDP publication and
local multicast reception using the bundled DLL. Its execution environment must
permit native UDP and child processes. This still does not replace dual-interface
or iPhone streaming acceptance.
