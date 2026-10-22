Fallout 2: A Post Nuclear Role Playing Game
PS2 Port - Classic Edition

Build: Classic Edition (fallout2-ce-ps2)
Version: 1.0.2 STABLE (release v1.0.2-ps2)
Platform: PlayStation 2 (PAL/NTSC, deteccion automatica)
Media: DVD-R / USB (mass:) / MX4SIO / HDD (PFS)

================================================================================
INSTALACION / INSTALLATION
================================================================================

USB / MX4SIO (RECOMENDADO):
1. Copia la carpeta 'FALLOUT2' a la raiz de tu USB formateado en FAT32
   (o a la particion MX4SIO).
2. Usa OPL / wLaunchELF / FMCB para lanzar FALLOUT2.ELF

DVD-R:
1. Graba la carpeta 'FALLOUT2' en un DVD-R (modo ISO9660 + UDF).
2. Arranca con ESR / OPL modo DVD.

HDD (PFS):
1. Copia la carpeta 'FALLOUT2' a tu particion PFS (ej. pfs:/FALLOUT2/).
2. Lanza desde OPL / HDLoader.

LAYOUT ESPERADO EN USB (FAT32 con MBR):
mass:/FALLOUT2/
  FALLOUT2.ELF       (este ejecutable)
  fallout2.cfg       (configuracion, se crea automaticamente)
  master.dat         (obligatorio, minusculas o mayusculas)
  critter.dat        (obligatorio, minusculas o mayusculas)
  data/              (opcional: mods, arte suelto, parches)
  patch000.dat       (opcional: parche oficial)
  PS2VIDEO.CFG       (opcional: forzar 'mode=pal' o 'mode=ntsc')
  saves/             (se crea solo al guardar)
  caps/              (capturas de video opcionales)

El port busca los datos en todos los mass disponibles
(mass:/, mass0:/, mass1:/, mass2:/, mass3:/), en mc0:/, cdrom0:/ y
hdd0:/FALLOUT2/, tanto en la raiz del dispositivo como dentro de una
carpeta FALLOUT2/. El VFS es case-insensitive: los archivos pueden estar
en minusculas, mayusculas o mixtas (el media oficial usa minusculas).

================================================================================
VIDEO / REGION
================================================================================

La region se detecta automaticamente desde la ROM de arranque:
Europa/PAL -> 640x448 a 50 Hz, resto -> 640x448 a 60 Hz.

Para forzarla, crea mass:/FALLOUT2/PS2VIDEO.CFG con una unica linea:

  mode=pal
  mode=ntsc

Si no estas seguro, no crees el archivo. PS2VIDEO.CFG tambien se acepta
en mc0:/FALLOUT2/ y mass0:/FALLOUT2/.

================================================================================
CONTROLES / CONTROLS
================================================================================

Left Stick / D-Pad      : Movimiento / Navegacion menus
Right Stick             : Camara (solo en exploracion)
Cross (X)               : Confirmar / Interactuar / Disparar
Circle (O)              : Cancelar / Correr (mantener)
Triangle                : Pip-Boy / Menu principal
Square                  : Inventario rapido / Arma
L1 / R1                 : Cambiar arma / Pestañas menu
L2 / R2                 : Apuntar (combate) / Scroll rapido
Select                  : Quick-save / Quick-load (configurable)
Start                   : Menu pausa / Opciones

================================================================================
AHORRO / SAVES
================================================================================

- Los saves se guardan en: mass:/FALLOUT2/saves/  (USB)
  o mc0:/FALLOUT2_S01..S10/   (Memory Card)
  o hdd0:/FALLOUT2/saves/     (HDD)

- En USB: cada partida crea su carpeta: SLOT_01/, SLOT_02/, etc.
- En Memory Card: ranuras FALLOUT2_S01..FALLOUT2_S10
- Incluye: .SAV (mapa), AUTOMAP.DB, AUTOMAP.TMP
- worldmap.dat se guarda en la raiz de FALLOUT2/

================================================================================
RENDIMIENTO / PERFORMANCE
================================================================================

- Objetivo: 60 FPS (NTSC) / 50 FPS (PAL) en menus e interiores
- Carga de mapa: < 2.5s (USB) / < 5s (DVD)
- Abrir UI (Pip-Boy, Inventario): < 150ms
- Emulador PCSX2: Usa modo "Strict" (sin speedhacks) para test real

Optimizaciones de la version 1.0:
- Cache direct-mapped de 1024 slots para art/cache lookup: reduce el
  frame time hasta un 36% en mapas densos (Den, Klamath, NCR, San Francisco).
- SIF RPC blindado: sin saltos a punteros de callback arbitrarios en
  _request_end; recuperacion determinista para fileio.
- Audio/VFS endurecidos: bounce buffer alineado + SyncDCache y cache de
  directorios ampliada.
- Binario de produccion Release stripped, sin contadores de instrumentacion.
- Validado 3/3 en perfil hyper-strict de PCSX2 (sin fastmem, FPU/VU clamp 3,
  0 TLB misses).

Referencia medida en PCSX2 strict (puede variar en hardware real):
- DENBUS1 ~52 FPS, NEWR1 ~43 FPS, SFTANKER ~43 FPS,
  KLAMALL ~38 FPS, NCRENT ~34 FPS.

================================================================================
PROBLEMAS CONOCIDOS / KNOWN ISSUES
================================================================================

- Intros MVE: Paleta restaurada correctamente tras video
- OSK (teclado virtual): Funciona para nombres de save
- Skilldex: Arreglado (antes TLB Miss / unaligned address)
- Tour de mapas: entre ~34 y ~52 FPS en PCSX2 strict segun el mapa
  (NCRENT es el mas pesado por su primer frame de carga)
- Si un mapa se cuelga en hardware real, adjunta el debug.log (ver abajo)

================================================================================
REPORTE DE ERRORES / CRASH REPORTS
================================================================================

El build de produccion mantiene el log de diagnostico del port PS2:

  mass:/FALLOUT2/debug.log   (USB / MX4SIO, destino normal)
  mass:/debug.log            (fallback si el juego esta en raiz del pendrive)
  mass0:/FALLOUT2/debug.log  (fallback mass0:)
  mass0:/debug.log           (fallback mass0: raiz)
  mc0:/FALLOUT2/debug.log    (Memory Card, fallback automatico)

Cada linea se escribe, se vuelca a disco y se cierra al instante, por lo que
ante un CUELLE o pantalla negra el archivo conserva la ultima actividad.

Tope de tamano: 4 MB. Si se supera, la redireccion se desactiva y se vuelve
al logger por linea (consola), para no llenar el pendrive.

IMPORTANTE - Colision con engine debug.mode=log:
El engine Fallout 2 CE abre 'debug.log' con modo "wt" en la raiz de datos
(cuando debug.mode=log en fallout2.cfg). Si usas USB y el engine tambien
escribe ahi, ambas escrituras comparten el mismo archivo en mass:/FALLOUT2/.
El port escribe lineas sueltas (flush inmediato); el engine hace fopen/fwrite/
fclose por linea. Ambas coexisten pero pueden entremezclar lineas.

Sincronizacion periodica (PS2_DEBUG_SYNC_MS = 5000):
Cada 5 s (en SDL_RenderPresent) se fuerza un cierre/reapertura del log
para vaciar la write-back cache de usbhdfsd y persistir el tamano en FAT.
Esto asegura que el debug.log sea legible tras un cuelgue repentino.

--- ARRANQUE BLINDADO EN PS2 SLIM (SCPH-700xx) ---
En hardware real, los drivers usbd.irx + usbhdfsd.irx tardan 1.5-4s en
negociar el pendrive USB 1.1 y montar mass:. Si el juego intenta leer
assets antes, hacia exit() limpio a boot.elf (pantalla negra sin debug.log).
Este build incluye:

  * Sondeo activo de mass:/mass0:/mass1:/mass2:/mass3: SIEMPRE, incluso si
    el loader ya monto mass: (6s, 20 intentos x 300ms), seguido de un
    fallback BDM/MX4SIO y otra espera de 6s.
  * Deteccion de dispositivo por stat()/open() en lugar de readdir(), para
    no disparar el HLE de usbhdfsd en PCSX2.
  * Redireccion de debug.log SOLO despues de confirmar el dispositivo.
  * Pila principal de 1 MB (-Wl,--defsym,_stack_size=0x100000) y reporte de
    high-water mark con canario al arrancar.
  * Vectores de excepcion R5900 propios: una excepcion muestra una pantalla
    fatal en la TV y vuelve al loader en lugar de colgarse en negro.
  * Watchdog de video de 15s: si el GS no presenta el primer frame, muestra
    un error claro (cable/region) en vez de quedarse en negro.
  * Tope de 4 MB para debug.log.

Si el juego crashea:
1. Copia debug.log del USB/Memory Card.
2. Indica modelo de PS2, metodo de carga (OPL/uLaunchELF/ESR),
   mapa donde ocurrio y version del ELF.
3. Adjunta el archivo en un issue:
   https://github.com/nicolasalucin/fallout2-ce-ps2/issues

En PCSX2 no se usa host: para el log (evita el bug de hostfs en Strict); en
el emulador la misma linea sale por consola y queda en el log de PCSX2.

================================================================================
CREDITOS / CREDITS
================================================================================

Original Game: Interplay / Black Isle Studios (1998)
Classic Edition Engine: fallout2-ce (GitHub)
PS2 Port: Nicolas Salucin (nicolasalucin)
  - Pipeline CT32 (Option A) - sin CLUT GS, lookup CPU
  - Overlay runtime COW (Copy-on-Write) en mass:/FALLOUT2_RT/
  - Heap 16MB engine + 8MB art cache + 32MB RDRAM budget
  - DMA sincronizado + double-buffer CLUT VRAM
  - Audio audsrv + IRX modules
  - Input DualShock 2 + pad emulation
  - v1.0: cache direct-mapped de art, guard SIF RPC, VFS/audio endurecidos
  - v1.0: telemetria de crash montada sobre mass:/ y mc0:/ (debug.log)
  - v1.0.1: arranque blindado PS2 Slim (sondeo mass:/ 4.5s, debug.log seguro,
    flush+pause 3s ante assets faltantes, _IONBF en stdout/stderr)
  - v1.0.2: VFS sin listados sinteticos, sondeo USB siempre activo,
    pila 1 MB, excepciones R5900 con pantalla fatal, guard SIF RPC completo
    (_fio_intr + _fio_read_intr), watchdog GS, log cap 4 MB

AI-Assisted Development:
  - Arquitectura bajo nivel (Gemini / DeepSeek / Claude via OpenCode)
  - Diagnosticos PS2SDK / gsKit / dmaKit
  - Maquinas de estado del smoke harness
  - Optimizaciones RDRAM / DMA / VRAM

================================================================================
SOPORTE / SUPPORT
================================================================================

GitHub:  https://github.com/nicolasalucin/fallout2-ce-ps2
Issues:  https://github.com/nicolasalucin/fallout2-ce-ps2/issues

Apoya el proyecto / Support the Project:
  - Cafecito: https://cafecito.app/nicolasalucin
  - Ko-fi:    https://ko-fi.com/nicolasalucin

================================================================================
LICENCIA / LICENSE
================================================================================

Este es un port de ingenieria inversa para preservacion y estudio.
Requiere copia original de Fallout 2 (GOG / Steam / CD).
No incluye assets originales - usa tu 'master.dat' / 'critter.dat'.