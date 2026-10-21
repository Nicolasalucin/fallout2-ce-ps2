# Local ps2sdk patches

These patches are applied to the **installed ps2sdk**, not to the game
sources. The CMake build consumes the patched `libkernel.a` from `$PS2SDK`.

## `ps2sdk-sifrpc-deferred.patch`

Target: `ee/kernel/src/sifrpc.c` in ps2sdk.

The stock SIF RPC completion handler dereferences `request->cd` and then
`cd->hdr.pkt_addr` from interrupt context. A malformed completion (observed
with PCSX2 as `cid=8000000a`, `_fio_cd`, `pkt_addr=0x00000000`) therefore
turns into an EE TLB miss at `pc=0x80003800`. Printing the packet directly
from the handler is not safe either: `printf` takes the stdio lock and can
deadlock if the interrupted thread owns it.

The patch adds:

- a pointer-range/alignment guard for `cd`;
- a guard that verifies `pkt_addr` belongs to the SIF RPC packet table and is
  packet-aligned before `rpc_packet_free()` is called;
- a strict end-function guard in the normal completion path. Only the known
  fileio callback is executed:
  * `cd == &_fio_cd` or `cd->end_function == _fio_intr` calls
    `_fio_intr(cd->end_param)`, so fileio receives its context pointer in
    `$a0` (the current ps2sdk stub ignores the argument, but the canonical
    fileio driver uses it);
  * any other non-NULL callback is **never** called from the interrupt
    handler. A corrupt pointer such as `0xffffffff` used to produce
    `PC=0xffffffff`; it is recorded for the deferred dump instead;
  * the waiting semaphore is still signalled and the packet is released, so
    a skipped callback cannot deadlock the caller;
- a fixed 16-word snapshot of the invalid completion, recorded in the
  handler without calling libc;
- `sceSifRpcDumpInvalidCompletion()`, which prints the snapshot later from
  thread context. The smoke harness calls it from `ps2_smoke_tick()` through
  a weak symbol, so an unpatched ps2sdk still links and runs.

A malformed completion cannot simply be dropped: the caller is blocked in
`WaitSema()` on the RPC client semaphore and would sleep forever (observed as
a deterministic hang in `dopen()` during `LoadGame`). When the client pointer
itself is valid but `pkt_addr` is not, the patch now:

- recovers the allocated packet from `cd->hdr.rpc_id` in the packet table;
- calls `_fio_intr(target->end_param)` only for the known fileio client (or
  the `_fio_cd` fallback when the completion lost its real client pointer);
- signals the client's semaphore and frees the recovered packet for fileio
  **and** for other clients, preventing a deadlock while never executing an
  arbitrary callback;
- publishes bind server fields, frees the recovered packet when found, and
  clears the client packet pointer;
- records the invalid completion for the deferred dump.

This keeps the TLB guard while ensuring a bad PCSX2 completion cannot leave a
blocking SIF caller stuck. The dump is printed from thread context on the
next smoke tick.

### Apply

From the root of a ps2sdk checkout (or the installed SDK source tree):

```sh
cd "$PS2SDK"
patch -p1 < /path/to/fallout2-ce-ps2/tools/patches/ps2sdk-sifrpc-deferred.patch
```

Rebuild and install the kernel library. The exact install target depends on
how ps2sdk was built; the important step is replacing the `SifRpcMain.o`
member in `$PS2SDK/ee/lib/libkernel.a`:

```sh
cd "$PS2SDK"
mips64r5900el-ps2-elf-gcc -O2 -G0 -g -D_EE -DF_SifRpcMain \
    -I"$PS2SDK/ee/include" -I"$PS2SDK/common/include" \
    -c ee/kernel/src/sifrpc.c -o /tmp/SifRpcMain.o
cp -p "$PS2SDK/ee/lib/libkernel.a" "$PS2SDK/ee/lib/libkernel.a.bak-pre-endguard"
mips64r5900el-ps2-elf-ar r "$PS2SDK/ee/lib/libkernel.a" /tmp/SifRpcMain.o
mips64r5900el-ps2-elf-ar s "$PS2SDK/ee/lib/libkernel.a"
mips64r5900el-ps2-elf-ranlib "$PS2SDK/ee/lib/libkernel.a"
```

Verify that the symbol made it into the archive and that the new object is
present:

```sh
mips64r5900el-ps2-elf-ar t "$PS2SDK/ee/lib/libkernel.a" | grep SifRpcMain.o
mips64r5900el-ps2-elf-nm "$PS2SDK/ee/lib/libkernel.a" \
    | grep sceSifRpcDumpInvalidCompletion
```

The diagnostic and recovery reference libkernel's `_fio_cd` client and the
`_fio_intr()` end function. If a future ps2sdk does not export them, remove
the `extern SifRpcClientData_t _fio_cd;` and
`extern void _fio_intr(void *);` declarations, the fileio recovery branch,
and the `_rpc_dump_client("_fio_cd", &_fio_cd);` call; the pointer guards,
strict callback guard, deferred dump, and generic packet validation remain
functional.
