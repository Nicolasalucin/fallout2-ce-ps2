# Fallout 2: Classic Edition — PS2 Port

[![Build](https://img.shields.io/badge/build-passing-brightgreen)]()
[![Platform](https://img.shields.io/badge/platform-PlayStation%202-blue)]()
[![License](https://img.shields.io/badge/license-Reverse%20Engineering%20%7C%20Preservation-orange)]()

> **Fallout 2: A Post Nuclear Role Playing Game** — Native PS2 port (ELF) running at 60 FPS NTSC, with full save/load, MVE playback, Pip-Boy, combat, world map, and all original gameplay systems.

---

## 📥 Installation

| Media | Method |
|-------|--------|
| **USB (mass:)** ✅ Recommended | Copy `FALLOUT2/` folder to FAT32 USB root. Launch `FALLOUT2.ELF` via OPL / wLaunchELF / FMCB. |
| **MX4SIO** ✅ Recommended | Same as USB, using MX4SIO adapter. |
| **DVD-R** | Burn `FALLOUT2/` folder as ISO9660+UDF. Boot via ESR / OPL DVD mode. |
| **HDD (PFS)** | Copy to `pfs:/FALLOOT2/`. Launch via OPL / HDLoader. |

> **Requires original game assets:** `master.dat`, `critter.dat`, `master.patch000.dat` (optional). Place in `FALLOUT2/data/` on your media.

---

## 🎮 Controls

| Button | Action |
|--------|--------|
| **Left Stick / D-Pad** | Movement / Menu navigation |
| **Right Stick** | Camera (exploration) |
| **Cross (✕)** | Confirm / Interact / Shoot |
| **Circle (○)** | Cancel / Run (hold) |
| **Triangle (△)** | Pip-Boy / Main Menu |
| **Square (□)** | Quick Inventory / Weapon |
| **L1 / R1** | Switch Weapon / Menu Tabs |
| **L2 / R2** | Aim (combat) / Fast Scroll |
| **Select** | Quick-Save / Quick-Load |
| **Start** | Pause Menu / Options |

---

## 💾 Saves

- **Location:** `mass:/FALLOUT2/` (USB) • `mc0:/FALLOUT2/` (Memory Card) • `hdd0:/FALLOUT2/` (HDD)
- **Per-slot folder:** `SLOT_01/`, `SLOT_02/`, etc.
- **Files:** `.SAV` (map), `AUTOMAP.DB`, `AUTOMAP.TMP`
- **World map:** `worldmap.dat` at `FALLOUT2/` root
- **Overlay runtime:** `FALLOUT2_RT/MAPS/` (Copy-on-Write, auto-managed)

---

## ⚡ Performance Targets

| Metric | Target | Notes |
|--------|--------|-------|
| **Menu / Interior FPS** | 55–60 FPS | NTSC 60 Hz |
| **Map Load** | < 1.5–2.5 s | USB / MX4SIO |
| **UI Open (Pip-Boy, Inv)** | < 150 ms | |
| **Combat Turn** | < 2 s | |
| **RDRAM Budget** | ≤ 32 MB | Engine 16 MB + Art 8 MB + System |

> Emulator (PCSX2): Run with **Strict profile** (no speedhacks, SyncMode=Disabled) for accurate timing. Emulator FPS ~12–25 is expected; real hardware hits 55–60 FPS.

---

## 🛠 Technical Highlights

| Feature | Implementation |
|---------|----------------|
| **Video Pipeline** | **Option A (CT32)** — CPU palette lookup (LUT), 1.2 MB CT32 texture/frame, **no GS CLUT**, no swizzle. Matches Fallout 1 CE. |
| **Palette/CLUT** | 256-entry LUT in EE RAM (`s_palette[256]`), GS CT32 byte order (ABGR). Forced re-upload after MVE/modal (`ps2_video_palette_mark_dirty`). |
| **VRAM** | Double-buffered CT32 texture (2 × 1.2 MB) + staging buffers. No T8/CLUT. |
| **DMA Sync** | `FlushCache(0)` + `dmaKit_wait_fast()` before every upload. |
| **Filesystem** | Overlay runtime `FALLOUT2_RT/` (COW). Reads from `mass:`/`host:`/`cdrom:`; writes (SAV, AUTOMAP) redirect to overlay. |
| **Save Confinement** | `.SAV` strictly under `MAPS/` — path remap fixed (`MAPS/` separator). |
| **Script Eviction** | `programListFree()` + extent purge in `scriptLoadAll` prevents heap fragmentation. |
| **Audio** | audsrv IRX + dedicated EE mixer thread (SPU2 streaming). |
| **Input** | DualShock 2 + virtual pad; event queue pumped from `SDL_RenderPresent`. |
| **Heap** | Engine 16 MB (`PS2_MEM_ENGINE_HEAP_MAX`). Art cache 8 MB (moveable blocks). Trim before large loads. |

---

## 🐛 Fixed Issues

| Issue | Fix |
|-------|-----|
| **Palette corruption (pink/black static)** | Migrated to CT32 pipeline (Option A). CPU LUT → CT32 texture. No GS CLUT, no CSM1/CSM2 swizzle. |
| **CLUT race (EE writes while DMA reads)** | Double-buffered VRAM CT32 + explicit DMA barrier (`dmaKit_wait_fast` + `FlushCache`). |
| **MVE palette not restored** | `ps2_video_palette_mark_dirty()` after movie end forces full LUT re-upload. |
| **Skilldex crash (TLB Miss → unaligned PC)** | Fixed struct alignment / function pointer in UI dispatcher. |
| **Save remap missing separator** | `_GameMap2Slot` now inserts `/` when `MAPS` prefix present. |
| **Heap OOM warnings (125 KB)** | Art cache trim before map load; eviction policy in `scriptLoadAll`. |

---

## 🤖 AI-Assisted Development

This port was developed with AI assistance across architecture, diagnostics, and optimization:

| Model | Role |
|-------|------|
| **Gemini** (via OpenCode) | Low-level architecture: EE/GS pipeline, DMA synchronization, VRAM allocation strategies, gsKit integration. |
| **DeepSeek** (via OpenCode) | PS2SDK diagnostics: heap analysis, IRX module loading, TLB exception tracing, `dmaKit`/`gsKit` behavior. |
| **Claude** (via OpenCode) | Smoke harness state machines, performance profiling, RDRAM budget allocation, DMA/CLUT race condition resolution. |

> All AI-generated code was reviewed, tested on hardware/emulator, and integrated manually. The port builds and runs on real PS2 (FMCB + OPL) and PCSX2 (Strict mode).

---

## 💝 Support the Project

If this port helps you relive the Wasteland on PS2, consider a coffee:

| Platform | Link |
|----------|------|
| ☕ **Cafecito** | <https://cafecito.app/nicolasalucin> |
| 💙 **Ko-fi** | <https://ko-fi.com/nicolasalucin> |

---

## 📂 Repository

- **Source:** <https://github.com/nicolasalucin/fallout2-ce-ps2>
- **Issues:** <https://github.com/nicolasalucin/fallout2-ce-ps2/issues>
- **Branch:** `ps2` (main development)

---

## ⚖️ Legal

This is a **reverse-engineering preservation project**. It does **not** include original game assets.

**You must own a legal copy of Fallout 2** (GOG, Steam, original CDs) to use this port. Copy your `master.dat`, `critter.dat`, and `master.patch000.dat` into `FALLOUT2/data/`.

Original game © 1998 Interplay / Black Isle Studios.  
Port © 2024–2025 Nicolas Salucin (nicolasalucin).

---

## 🇪🇸 Versión en Español

### Fallout 2: Classic Edition — Port nativo PS2

> **Fallout 2: Un Juego de Rol Post-Nuclear** — Port nativo PS2 (ELF) a 60 FPS NTSC, con guardado/carga completo, reproducción MVE, Pip-Boy, combate, mapa mundial y todos los sistemas de juego originales.

---

### 📥 Instalación

| Medio | Método |
|-------|--------|
| **USB (mass:)** ✅ Recomendado | Copia la carpeta `FALLOUT2/` a la raíz de tu USB FAT32. Lanza `FALLOUT2.ELF` con OPL / wLaunchELF / FMCB. |
| **MX4SIO** ✅ Recomendado | Igual que USB, usando adaptador MX4SIO. |
| **DVD-R** | Graba `FALLOUT2/` como ISO9660+UDF. Arranca con ESR / OPL modo DVD. |
| **HDD (PFS)** | Copia a `pfs:/FALLOUT2/`. Lanza con OPL / HDLoader. |

> **Requiere assets originales:** `master.dat`, `critter.dat`, `master.patch000.dat` (opcional). Ponlos en `FALLOUT2/data/` en tu medio.

---

### 🎮 Controles

| Botón | Acción |
|-------|--------|
| **Stick Izq / D-Pad** | Movimiento / Navegación menús |
| **Stick Der** | Cámara (exploración) |
| **Cruz (✕)** | Confirmar / Interactuar / Disparar |
| **Círculo (○)** | Cancelar / Correr (mantener) |
| **Triángulo (△)** | Pip-Boy / Menú principal |
| **Cuadrado (□)** | Inventario rápido / Arma |
| **L1 / R1** | Cambiar arma / Pestañas menú |
| **L2 / R2** | Apuntar (combate) / Scroll rápido |
| **Select** | Quick-Save / Quick-Load |
| **Start** | Menú pausa / Opciones |

---

### 💾 Partidas

- **Ubicación:** `mass:/FALLOUT2/` (USB) • `mc0:/FALLOUT2/` (Memory Card) • `hdd0:/FALLOUT2/` (HDD)
- **Por slot:** `SLOT_01/`, `SLOT_02/`, etc.
- **Archivos:** `.SAV` (mapa), `AUTOMAP.DB`, `AUTOMAP.TMP`
- **Mapa mundial:** `worldmap.dat` en la raíz de `FALLOUT2/`
- **Overlay runtime:** `FALLOUT2_RT/MAPS/` (Copy-on-Write, automático)

---

### ⚡ Objetivos de Rendimiento

| Métrica | Objetivo | Notas |
|---------|----------|-------|
| **FPS Menús / Interiores** | 55–60 FPS | NTSC 60 Hz |
| **Carga de Mapa** | < 1.5–2.5 s | USB / MX4SIO |
| **Abrir UI (Pip-Boy, Inv)** | < 150 ms | |
| **Turno Combate** | < 2 s | |
| **Presupuesto RDRAM** | ≤ 32 MB | Engine 16 MB + Art 8 MB + Sistema |

> Emulador (PCSX2): Usa perfil **Strict** (sin speedhacks, SyncMode=Disabled) para tiempos reales. En emulador ~12–25 FPS es normal; hardware real llega a 55–60 FPS.

---

### 🛠 Detalles Técnicos

| Característica | Implementación |
|----------------|----------------|
| **Pipeline Video** | **Opción A (CT32)** — Lookup CPU (LUT), textura CT32 1.2 MB/frame, **sin CLUT GS**, sin swizzle. Igual que Fallout 1 CE. |
| **Paleta/CLUT** | LUT 256 entradas en EE RAM (`s_palette[256]`), orden CT32 GS (ABGR). Re-subida forzada tras MVE/modal (`ps2_video_palette_mark_dirty`). |
| **VRAM** | Double-buffered CT32 (2 × 1.2 MB) + staging. Sin T8/CLUT. |
| **Sync DMA** | `FlushCache(0)` + `dmaKit_wait_fast()` antes de cada subida. |
| **Filesystem** | Overlay runtime `FALLOUT2_RT/` (COW). Lecturas `mass:`/`host:`/`cdrom:`; escrituras (SAV, AUTOMAP) redirigidas a overlay. |
| **Confinamiento Save** | `.SAV` estrictamente bajo `MAPS/` — remap arreglado (separador `MAPS/`). |
| **Eviction Scripts** | `programListFree()` + purga extents en `scriptLoadAll` evita fragmentación heap. |
| **Audio** | audsrv IRX + hilo mixer EE dedicado (streaming SPU2). |
| **Input** | DualShock 2 + pad virtual; cola eventos bombeada desde `SDL_RenderPresent`. |
| **Heap** | Engine 16 MB (`PS2_MEM_ENGINE_HEAP_MAX`). Art cache 8 MB (bloques móviles). Trim antes de cargas grandes. |

---

### 🐛 Issues Arreglados

| Problema | Solución |
|----------|----------|
| **Corrupción paleta (estática rosa/negro)** | Migración a pipeline CT32 (Opción A). Lookup CPU → textura CT32. Sin CLUT GS, sin CSM1/CSM2. |
| **Race CLUT (EE escribe mientras DMA lee)** | Double-buffer VRAM CT32 + barrera DMA explícita (`dmaKit_wait_fast` + `FlushCache`). |
| **Paleta MVE no se restauraba** | `ps2_video_palette_mark_dirty()` tras fin de movie fuerza re-subida completa LUT. |
| **Crash Skilldex (TLB Miss → PC unaligned)** | Arreglada alineación struct / puntero función en despachador UI. |
| **Remap save sin separador** | `_GameMap2Slot` inserta `/` cuando hay prefijo `MAPS`. |
| **Warnings Heap OOM (125 KB)** | Trim art cache antes de carga mapa; política evicción en `scriptLoadAll`. |

---

### 🤖 Desarrollo Asistido por IA

Este port se desarrolló con asistencia de IA en arquitectura, diagnóstico y optimización:

| Modelo | Rol |
|--------|-----|
| **Gemini** (vía OpenCode) | Arquitectura bajo nivel: pipeline EE/GS, sincronización DMA, estrategias VRAM, integración gsKit. |
| **DeepSeek** (vía OpenCode) | Diagnósticos PS2SDK: análisis heap, carga módulos IRX, trazado excepciones TLB, comportamiento `dmaKit`/`gsKit`. |
| **Claude** (vía OpenCode) | Máquinas de estado smoke harness, profiling rendimiento, presupuesto RDRAM, resolución race conditions DMA/CLUT. |

> Todo código generado por IA fue revisado, probado en hardware/emulador e integrado manualmente. El port compila y corre en PS2 real (FMCB + OPL) y PCSX2 (modo Strict).

---

### 💝 Apoya el Proyecto

Si este port te ayuda a revivir el Yermo en PS2, invítame un café:

| Plataforma | Enlace |
|------------|--------|
| ☕ **Cafecito** | <https://cafecito.app/nicolasalucin> |
| 💙 **Ko-fi** | <https://ko-fi.com/nicolasalucin> |

---

### 📂 Repositorio

- **Código:** <https://github.com/nicolasalucin/fallout2-ce-ps2>
- **Issues:** <https://github.com/nicolasalucin/fallout2-ce-ps2/issues>
- **Rama:** `ps2` (desarrollo principal)

---

### ⚖️ Legal

Este es un proyecto de **preservación mediante ingeniería inversa**. **No incluye assets originales**.

**Debes poseer una copia legal de Fallout 2** (GOG, Steam, CDs originales) para usar este port. Copia tus `master.dat`, `critter.dat` y `master.patch000.dat` a `FALLOUT2/data/`.

Juego original © 1998 Interplay / Black Isle Studios.  
Port © 2024–2025 Nicolas Salucin (nicolasalucin).