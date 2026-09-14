# Network store queries

The Windows socket backend answers the three read-only NSI operations through the host's system `nsi.dll`: `NsiGetParameter`, `NsiGetAllParameters`, and `NsiEnumerateObjectsAllParameters`. Guest instructions still execute in the emulator. The backend receives owned buffers, never guest virtual addresses. No NSI write, registration, or notification operation is forwarded.

The device marshals native x64 and WoW64 requests separately. Lengths and enumeration counts are 32-bit fields even in the x64 request. Each query has a 16 MiB aggregate buffer limit. Query keys remain unchanged; enumeration keys are outputs. Static interface data, dynamic state, and counters are returned alongside the read/write parameter block. Partial enumeration preserves the caller's capacity and returns the required count.

Interface IDs, link state, routes, and counters describe the host networking used by the socket backend. Counters include host traffic; they are not measurements of guest-only packets. Network state is queried at the time of the request and can change across snapshot replays. Restoring a snapshot does not restore the host's adapter state.

The static socket backend and non-Windows hosts currently return unsupported for NSI queries. An injected socket backend can implement `query_network_store` for a deterministic network model. Unsupported NSI controls return `STATUS_NOT_SUPPORTED` instead of reporting a fabricated success.

The x64 request offsets were checked against the guest Windows 10.0.19045 `nsi.dll` and actual `iphlpapi.dll!InternalGetIfEntry2Ex` calls. The WoW64 offsets were checked against `SysWOW64\nsi.dll`. Interface field definitions are documented in [MIB_IF_ROW2](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/ns-netioapi-mib_if_row2).

Regression coverage checks nested output buffers, unchanged query keys, static/dynamic data boundaries, enumeration capacity, partial results, error preservation, and rejected malformed requests. Actual-game acceptance must be checked separately from those tests.
