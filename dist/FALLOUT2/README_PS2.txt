Fallout 2: A Post Nuclear Role Playing Game
PS2 Port - Classic Edition

Build: Classic Edition (fallout2-ce-ps2)
Platform: PlayStation 2 (NTSC)
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

- Los saves se guardan en: mass:/FALLOUT2/  (USB)
  o mc0:/FALLOUT2/       (Memory Card)
  o hdd0:/FALLOUT2/      (HDD)

- Cada partida crea su carpeta: SLOT_01/, SLOT_02/, etc.
- Incluye: .SAV (mapa), AUTOMAP.DB, AUTOMAP.TMP
- worldmap.dat se guarda en la raiz de FALLOUT2/

================================================================================
RENDIMIENTO / PERFORMANCE
================================================================================

- Objetivo: 60 FPS (NTSC) en menus e interiores
- Carga de mapa: < 2.5s (USB) / < 5s (DVD)
- Abrir UI (Pip-Boy, Inventario): < 150ms
- Emulador PCSX2: Usa modo "Strict" (sin speedhacks) para test real

================================================================================
PROBLEMAS CONOCIDOS / KNOWN ISSUES
================================================================================

- Intros MVE: Paleta restaurada correctamente tras video
- OSK (teclado virtual): Funciona para nombres de save
- Skilldex: Arreglado (antes TLB Miss / unaligned address)
- Tour de mapas: ~13 FPS en emulador (target 45 FPS en hardware real)

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