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
- a fixed 16-word snapshot of the invalid completion, recorded in the
  handler without calling libc;
- `sceSifRpcDumpInvalidCompletion()`, which prints the snapshot later from
  thread context. The smoke harness calls it from `ps2_smoke_tick()` through
  a weak symbol, so an unpatched ps2sdk still links and runs.

### Apply

From the root of a ps2sdk checkout (or the installed SDK source tree):

```sh
cd "$PS2SDK"
patch -p1 < /path/to/fallout2-ce-ps2/tools/patches/ps2sdk-sifrpc-deferred.patch
```

Rebuild and install the kernel library:

```sh
cd "$PS2SDK/ee/kernel"
make clean
make -j"$(nproc)"
make install
```

Verify that the symbol made it into the archive:

```sh
mips64r5900el-ps2-elf-nm "$PS2SDK/ee/lib/libkernel.a" \
    | grep sceSifRpcDumpInvalidCompletion
```

The diagnostic references libkernel's `_fio_cd` client to print the fileio
client state. If a future ps2sdk does not export that symbol, remove the
`extern SifRpcClientData_t _fio_cd;` declaration and the
`_rpc_dump_client("_fio_cd", &_fio_cd);` call; the safety guards and the
deferred dump remain functional.
