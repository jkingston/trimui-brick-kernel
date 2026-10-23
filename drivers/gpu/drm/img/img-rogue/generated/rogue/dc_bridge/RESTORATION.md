Display-class compatibility
===========================

The StarFive DDK 1.19@6345021 source omits display class. This implementation
adapts the licensed Allwinner DDK 1.11 code from Creality commit
7f37a7fbf4ad59907034715e3d967b9177de1cd4 to the Brick's DDK 1.19 interfaces.
Original copyright and license notices are retained. It is not an original
Imagination DDK 1.19 display-class release.

The bridge matches the stock 1.1.1 ABI. The implementation uses modern PMR,
handle, fence and timeline interfaces. DCUnregisterDeviceChecked adds a drain
contract: RETRY retains registration; OK permits teardown. Callers must
serialize teardown and stop using the handle after OK.

Validated use is NULL_WSEGL pbuffer rendering with the metadata adapter.
Scanout, exceptional fence paths and teardown under load remain unqualified.
