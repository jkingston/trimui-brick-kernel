# Display-class reconstruction candidate

Stock Brick firmware 1.1.1 pvrsrvkm.ko is DDK 1.19@6345021, with DWARF.
The public StarFive 1.19 tree omits display class. These sources restore the
licensed Creality Allwinner DDK 1.11 implementation, adapting its real buffer,
PMR and fence operations to 1.19. This is not an upstream 1.19 source release.

Evidence in ../../../../../../../../../../firmware/metadata/ includes stock
function signatures, callback DWARF, DC bridge size/offset assertions and
focused stock disassembly. The common bridge header's 52 structs match stock
DWARF field types, sizes and offsets. Provider callbacks remove obsolete heap
ID outputs and add the stock ResetDevice callback. Configure2 uses the stock
44-byte input and 8-byte output; server-sync arrays and client-cache sequence
were removed, while acquire/release fence and timeline semantics remain.

DC core uses the device display heap (falling back to GPU-local only for
PHYSHEAP_ID_INVALID), matching stock registration, and the modern PMR API.
DC dispatch uses staged handle destruction; partially acquired lookup arrays
are zeroed and only successful lookups are released. All 26 DC dispatch
commands are registered. Linux 7.3 cross-compilation passed independently for
DC core and dispatch. The full module and hardware remain separate gates.

Outstanding semantic review: the legacy core retains a singly-linked device
list and first-GPU device association (the target has one GPU), whereas stock
uses newer list helpers and indexed device lookup. The new ResetDevice
callback is represented accurately but no new recovery behavior is inferred.
Configure scheduling and exceptional fence paths require further comparison
and device testing. Compilation and ABI layout checks do not establish GPU
execution, cache coherency, correct display timing or recovery safety.

## Follow-up semantic audit

Focused stock disassembly established and corrected GPU association using
`PVRSRVGetDeviceInstance(g_ui32DCDeviceCount)`, queue-full MISR scheduling,
PMR-creation failure reference release, cleared physical-address arrays after
free, and optional `DCResetDevice` delegation (absent callback returns error10,
NOT_IMPLEMENTED). Device acquisition now checks both index and GPU identity
while holding the list lock. Stock flush uses the retained 500-iteration SCP
loop and final null flip. Stock InitDCBridge registers all26 handlers with a
NULL bridge lock; the restored registration matches.

A deliberately new API, `DCUnregisterDeviceChecked`, supports safe adapter
teardown: RETRY keeps registration alive, and OK means no outstanding device
references, removal from discovery, and completed freeing. It does not infer
success from unhealthy-services state. The legacy void API remains for ABI
compatibility and must not be used by the new adapter as a drain guarantee.
Reference release holds the list lock through event signaling, preventing a
concurrent checked teardown from freeing the event between decrement and
signal. The system-buffer cache has a per-device mutex shared by acquisition
and checked teardown; detaching resets both PMR and buffer-handle cache before
releasing its reference. Callers must serialize teardown attempts and stop
using the handle after OK. These added synchronization semantics need stress
and fault-injection tests on the eventual supported device.

The earlier first-device and absent reset-helper gaps are now resolved. The
private device list remains singly linked, with external ordering/identity
semantics maintained; no externally visible structure depends on its layout.
