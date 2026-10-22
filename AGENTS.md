# AGENTS.md — Fallout 2 CE PS2 Agent Rules

Este archivo es el contrato único para cualquier agente que trabaje en este repositorio. Combina las **reglas técnicas de arquitectura PS2** extraídas de `ps2-forge` (engine minimalista, un header, gsKit/libpad) con las **reglas obligatorias del proyecto** (hardware real, build reproducible, sin workarounds de emulador en producción).

---

## 1. Mental Model y Arquitectura PS2 (de ps2-forge)

| Concepto | Regla |
|---|---|
| **EE (Emotion Engine)** | MIPS R5900, 32 MB RDRAM, FPU single-precision (usa `-mno-check-zero-division`). |
| **GS (Graphics Synthesizer)** | VRAM 4 MB, DMA por `dmaKit` + `gsKit`. Texturas CT32 alineadas a 64 bytes. |
| **IOP** | Corre módulos IRX (`usbd.irx`, `usbhdfsd.irx`, `sio2man`, `pad`, `audsrv`). Comunicación vía SIF RPC. |
| **DMA / Alineación** | Buffers DMA: **16-byte aligned** mínimo; texturas GS: **64-byte aligned**. Usa `__attribute__((aligned(64)))`. |
| **Caché de datos** | `FlushCache(0)` / `FlushCache(2)` antes de DMA desde CPU; `SyncD(0)` / `SyncD(2)` después. |
| **Coordenadas virtuales** | ps2-forge usa **320×240 virtual**; nosotros renderizamos **640×480 engine → 640×448 GS** (PAL/NTSC). |
| **FPU R5900** | Solo **single-precision** nativo; `double` emulado (lento). Compila con `-ffast-math -mno-check-zero-division`. |
| **IRX / Módulos** | Enlaza IRX como arrays C generados por CMake (`ps2_*_irx_data.c`). Carga con `SifExecModuleBuffer`/`SifLoadModule`. |
| **Audio** | `audsrv.irx` + ADPCM. Inicializa **una vez** en `init`. |
| **Input** | `libpad` en IOP, sondea cada frame; detecta edges (held/just_pressed/just_released). |

---

## 2. Reglas Obligatorias del Proyecto (Hardware Real)

### 2.1 Firmas y SDK — **Prohibido adivinar**
- **Siempre** verifica firmas con `grep -rn "función" $PS2SDK/ee/include` antes de usarlas.
- No asumas parámetros ni tipos; el SDK ps2dev cambia entre versiones.
- Usa `configure_file("$ENV{PS2SDK}/ee/include/debug.h" ...)` para evitar colisión con `src/debug.h` del engine.

### 2.2 I/O en PS2 Real — **Sin workarounds de emulador en producción**
| Qué | Correcto | Prohibido en producción |
|---|---|---|
| Detectar dispositivo | `stat()` + `open()` (`O_RDONLY`) | `opendir()`/`readdir()` (dispara HLE bug en PCSX2) |
| Sondear `mass:` | `stat("mass:/")` en bucle activo | `dopen("mass:/")` o listados sintéticos |
| Flush FAT32 | `close(STDOUT); close(STDERR); debug_redirect_open(path)` (re-abre con `O_APPEND`) | Solo `fflush()` (no actualiza FAT) |
| Descriptores 1/2 | **Nunca** dejes `STDOUT_FILENO`/`STDERR_FILENO` cerrados; salva/restaura con `dup()`/`dup2()` | `close(1)` sin `dup2` de vuelta a consola |
| SIF RPC guard | Acepta **ambos** `_fio_intr` **y** `_fio_read_intr` | Solo `_fio_intr` (rompe reads completados) |
| `mc0:` | `storage_wait_dir("mc0:", 10, 100)` tras USB | Retardo fijo de 10 s |

### 2.3 Stack y Flags de Enlace
```cmake
# toolchain-ps2.cmake
-Wl,--defsym,_stack_size=0x100000     # 1 MB main stack
-mno-check-zero-division              # evita trap div/0 en FPU
```

### 2.4 VFS Limpio
- `s_probed = 1` **solo** cuando `s_root_count > 0` (raíz real encontrada).
- Listados sintéticos `mass:/` y `mass:/FALLOUT2/` **solo** bajo `PS2_PCSX2_USB_MANIFEST=ON` (default OFF).
- Raíces reales: `mass:`..`mass3:`, `mc0:`, `cdrom0:`, `hdd0:/FALLOUT2/`, `host:`.
- `ps2_fs_reprobe()` disponible para re-scan tras montaje tardío.

### 2.5 Logger Resistente a Crashes
- `ps2_debug_sync()`: `O_APPEND` + `dup2()` + **salva** `fd 1/2` con `dup()` + **restaura** al desactivar.
- Tope 4 MB **real**: flag pegajoso `s_log_capped`; per-line logger **no** re-abre el mismo archivo; fallback a consola.
- Crash mode: `ps2_debug_enter_crash_mode()` → `PS2_LOG` solo espejo a consola, sin tocar FS.

### 2.6 Video / GS Hardening
- `gsKit_init_global()` **validado** (no NULL).
- VRAM texture: `gsKit_vram_alloc(gs, size, GSKIT_ALLOC_USERBUFFER)` (no `GS_VRAM_BLOCKSIZE_256`).
- PAL/NTSC: `GetRomName()` → `romname[4]=='E'`; override con `PS2VIDEO.CFG` (`mode=pal|ntsc`) en `mass:/`, `mass0:/`, `mc0:/`.
- Watchdog 15 s: si `!s_first_present_done` → `ps2_fatal_show("GS PRESENT TIMEOUT", ...)`.

### 2.7 Excepciones y Salida
- Vectores R5900 (VTLB refill + common) instalados **antes** de cualquier I/O.
- Pantalla fatal vía `libdebug` raw GS (no gsKit); `ps2_fatal_show()` → `ps2_exit_to_loader()` → `LoadExecPS2("rom0:OSDSYS")` / `ResetEE(0)`.
- `-ldebug` **antes** que `-lgraph` en `target_link_libraries`.

### 2.8 Gate de Release Obligatorio (`tools/make_dist.sh`)
Antes de publicar **cualquier** release:
```bash
tools/make_dist.sh <version> build_ps2_prod
```
El script verifica y **falla** si:
- ELF de producción **no** es más nuevo que todas las fuentes (`find src cmake CMakeLists.txt -newer ELF`).
- `grep -aq "SUITE_FINISHED"` en el ELF (restos de smoke test).
- Falta alguna **hardening string** en el ELF:
  - `waiting for mass`
  - `mass:/debug.log`
  - `R5900 vectors installed`
  - `stack: top=`
  - `GS PRESENT TIMEOUT`
  - `gsKit could not reserve`
- `cmp -s build_elf dist_elf` (bit-a-bit idénticos → SHA-256 coincidentes).
- Imprime SHA-256 de build ELF, dist ELF y ZIP.

---

## 3. Build y Test Loop

```bash
# Configura (una vez por build dir)
cmake -B build_ps2_prod -DCMAKE_BUILD_TYPE=Release -DPS2_DEV_SMOKE_TEST=OFF -DPS2_STRIP_ELF=ON \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-ps2.cmake -DPS2_BUILD=ON

# Compila
cmake --build build_ps2_prod -j$(nproc)

# Smoke estricto (solo para validar, NO en release)
PS2_SMOKE_TIMEOUT=900 PS2_SMOKE_FASTMEM=0 tools/run_smoke.sh build_ps2_smoke_v1
# Debe salir: SUITE_FINISHED_SUCCESS, 0 TLB misses, 0 faults, stack 1024K canary=ok

# Empaqueta y publica
tools/make_dist.sh 1.0.2 build_ps2_prod
gh release create v1.0.2-ps2 dist/fallout2-ce-ps2-v1.0.2.zip --latest --title "..." --notes-file ...
```

---

## 4. Checklist Rápido Pre-Commit

| ✅ | Verificación |
|---|---|
| 1 | `grep -rn "nueva_función" $PS2SDK/ee/include` pasó |
| 2 | Sin `opendir()` en código de producción (`ps2_storage.c`, `fs.c`) |
| 3 | `stat()`/`open()` para sondeo de dispositivos |
| 4 | `ps2_debug_sync()` usa `O_APPEND` + `dup2()` + salva/restaura fd 1/2 |
| 5 | `GSKIT_ALLOC_USERBUFFER` en `video_gs.c` |
| 6 | `s_probed` solo `1` con `s_root_count > 0` |
| 7 | Tope 4 MB con `s_log_capped` (no re-abre archivo capado) |
| 8 | `-Wl,--defsym,_stack_size=0x100000` + `-mno-check-zero-division` |
| 9 | SIF guard acepta `_fio_intr` y `_fio_read_intr` |
| 10 | `tools/make_dist.sh` pasa sin errores |

---

## 5. Referencias y Archivos Clave

| Archivo | Qué controla |
|---|---|
| `cmake/toolchain-ps2.cmake` | Stack 1 MB, flags MIPS, ASM, linker flags |
| `src/platform/ps2/ps2_storage.c` | Sondeo USB activo, `stat()`/`open()`, `storage_wait_dir` |
| `src/platform/ps2/fs.c` | VFS raíces reales, `s_probed`, `ps2_fs_reprobe`, manifest OFF |
| `src/platform/ps2/ps2_debug.c` | Logger 4 MB cap, `s_log_capped`, crash mode, sync seguro |
| `src/platform/ps2/video_gs.c` | PAL/NTSC, `PS2VIDEO.CFG`, VRAM validado, watchdog 15 s |
| `src/platform/ps2/ps2_exception_asm.S` | Vectores R5900, frame layout, stack dedicado |
| `src/platform/ps2/ps2_exit.c` | `ps2_fatal_show`, `ps2_exit_to_loader`, loader fallback |
| `tools/patches/ps2sdk-sifrpc-deferred.patch` | Guard SIF `_fio_read_intr` + `_fio_intr` |
| `tools/make_dist.sh` | Gate de release reproducible |

---

## 6. Notas de Hardware Real (No Validado en Esta Release)

> **Advertencia:** La release `v1.0.2-ps2` **no ha sido probada en PS2 físico (SCPH-700xx)**. Toda la validación fue en PCSX2 strict (`PS2_SMOKE_FASTMEM=0`). El código está preparado para hardware real (sondeo USB 6 s + fallback BDM, stack 1 MB, vectores de excepción, watchdog GS), pero **cualquier tester que lo ejecute en hardware real debe reportar `debug.log` y comportamiento de arranque**.

---

## 7. Flujo de Publicación (Resumen)

1. Código → commits atómicos → push rama `ps2`.
2. Smoke estricto verde (`SUITE_FINISHED_SUCCESS`, 0 TLB/faults).
3. `tools/make_dist.sh <ver> build_ps2_prod` → ZIP + SHA-256.
4. `git tag -a v<ver>-ps2 -m "..." && git push origin ps2 && git push origin v<ver>-ps2`.
5. `gh release create v<ver>-ps2 dist/fallout2-ce-ps2-v<ver>.zip --latest --title "..." --notes-file ...`.
6. Verificar asset subido: `gh release view v<ver>-ps2 -R <fork> --json assets` → SHA-256 coincide.

---

**Última validación (v1.0.2-ps2):**
- Build ELF = dist ELF = ZIP ELF = GitHub ZIP ELF: `628356fcf6902bad25bad3d641f4351a5b9db6fb279f9cc41c75cd8942d01873`
- ZIP SHA-256: `dd9f064358911fbb113f14fba42576691bd1883aaf8481b9748d58b4db00608c`
- Smoke: `SUITE_FINISHED_SUCCESS`, 0 TLB misses, 0 faults, 8 maps, stack 1024K canary=ok
- Release publicada: https://github.com/Nicolasalucin/fallout2-ce-ps2/releases/tag/v1.0.2-ps2