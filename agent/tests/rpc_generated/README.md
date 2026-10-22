# Generated RPC Provider conformance

The native CTest `test_turboagent_typed_rpc` uses the installed `salts-idlc`,
DataBind MethodPlan, CHttp Client/Server and a separately loaded Provider DSO.
It runs in the normal Linux/Windows test graph. Cross-compilation does not
establish native execution or DLL unload acceptance.

The generation-rollover scenario drains and destroys the first Server, unloads
its DSO, then reloads the Provider in the **same live Registry**. Capacity one
forces the same slot to acquire a different generation. All old manifest,
export, offer and callback borrows end before unload. The replacement callback
is obtained under a fresh discovery lease; host-owned native descriptors and
the MethodPlan remain valid independently of either Provider generation.

An acquire using the old numeric Ref must return `CMETA_PLUGIN_STALE` with zero
lease and null manifest. Mounting with that Ref and the currently leased
replacement code must return `SALTS_EBUSY`, CHttp's published mapping for failed
Plugin acquisition. The rejection must invoke no Provider, leak no lease and
consume no method slot. Mounting the same method with the current Ref must then
succeed, execute the real replacement DSO exactly once and return typed sum 11.
The replacement Server must retain its lease until drain/destroy; final unload
requires zero leases and quiescence.

This is an explicit Host reload after complete old-generation drain. It does
not test cross-Registry destroy/reinit fencing (Salts #1156), automatic rebinding,
live hot replacement, arbitrary process kill or full dependency sanitizers.
Official corrected typed-error package qualification remains in #122; full
source instrumentation remains in #123.
