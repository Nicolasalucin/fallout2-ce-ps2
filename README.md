# Fallout 2 Community Edition for PlayStation 2

**Fallout 2 Community Edition for PlayStation 2** — Re-implementación optimizada para hardware real de PS2 y PCSX2, basada en el trabajo de **Alexey Batalov** y porteada por **Nicolas Alucin**.

> Port nativo PS2 (ELF) a 60 FPS NTSC con: guardado/carga completo, reproducción MVE, Pip-Boy, combate, mapa mundial y todos los sistemas de juego originales.

---

## 📥 Requisitos e Instalación en PS2

### Archivos necesarios en el medio de almacenamiento (USB / MX4SIO / HDD / DVD)

```
FALLOUT2/
├── FALLOUT2.ELF          # Ejecutable principal
├── SYSTEM.CNF            # Configuración de arranque PS2
├── README_PS2.txt        # Instrucciones en consola
├── MASTER.DAT            # Datos principales (requerido)
├── CRITTER.DAT           # Datos de criaturas (requerido)
├── PATCH000.DAT          # Parche oficial (opcional, recomendado)
└── data/                 # Carpeta con assets del juego
    ├── art/
    ├── maps/
    ├── proto/
    ├── sound/
    ├── text/
    └── ...
```

> **Importante:** Debes poseer una copia legal de Fallout 2 (GOG, Steam, CDs originales). Copia tus archivos `.DAT` y la carpeta `data/` a `FALLOUT2/` en tu medio.

### Métodos de carga soportados

| Medio | Herramienta | Notas |
|-------|-------------|-------|
| **USB (mass:)** | **OPL** / uLaunchELF / wLaunchELF / FMCB | FAT32, cluster 32 KB recomendado |
| **MX4SIO** | **OPL** / uLaunchELF | Adaptador SD a Memory Card slot |
| **HDD (PFS)** | **OPL** / HDLoader | Partición PFS formateada |
| **SMB (Red)** | **OPL** | Requiere PC/NAS compartiendo carpeta |
| **DVD-R** | ESR / OPL (modo DVD) | ISO9660 + UDF, velocidad 4x |

### Guardado de partidas (Saves)

- **Memory Card:** `mc0:/FALLOUT2_S01/` ... `mc0:/FALLOUT2_S10/`
- **USB / HDD:** `mass:/FALLOUT2/SLOT_01/` ... `mass:/FALLOUT2/SLOT_10/`
- **Estructura por slot:**
  ```
  SLOT_01/
  ├── ARTEMPLE.SAV          # Mapa actual
  ├── AUTOMAP.DB            # Automapa
  ├── AUTOMAP.TMP           # Temporal automapa
  ```
- **Mapa mundial:** `FALLOUT2/worldmap.dat` (raíz del medio)
- **Overlay runtime (COW):** `FALLOUT2_RT/MAPS/` — escritura automática, no tocar manualmente

---

## 🎮 Mapeo de Controles (DualShock 2)

| Botón / Eje | Acción |
|-------------|--------|
| **Análogo Izquierdo / D-Pad** | Movimiento del cursor / navegación menús |
| **Cruz (X)** | Click izquierdo / Confirmar / Interactuar / Disparar |
| **Círculo (O)** | Click derecho / Alternar modo cursor / Correr (mantener) |
| **Cuadrado (□)** | Inventario / Menú contextual de acciones |
| **Triángulo (△)** | Menú opciones / Cancelar / Salir / Pip-Boy |
| **L1 / R1** | Cambio de arma / Pip-Boy / Mapa mundial |
| **L2 / R2** | Apuntar (combate) / Scroll rápido listas |
| **Start** | Pausa / Menú principal |
| **Select** | Teclado en pantalla (OSK) / Quick-Save |

> El OSK (On-Screen Keyboard) permite escribir nombres de saves y buscar en el Pip-Boy.

---

## ⚡ Rendimiento (Targets)

| Métrica | Objetivo | Notas |
|---------|----------|-------|
| **FPS Menús / Interiores** | 55–60 FPS | NTSC 60 Hz |
| **Carga de Mapa** | < 1.5–2.5 s | USB / MX4SIO |
| **Abrir UI (Pip-Boy, Inv)** | < 150 ms | |
| **Turno Combate** | < 2 s | |
| **Presupuesto RDRAM** | ≤ 32 MB | Engine 16 MB + Art 8 MB + Sistema |

> **Emulador PCSX2:** Usa perfil **Strict** (sin speedhacks, `SyncMode=Disabled`, `vuFlagHack=false`) para tiempos reales. En emulador ~12–25 FPS es normal; **hardware real alcanza 55–60 FPS**.

---

## 🛠 Detalles Técnicos

| Característica | Implementación |
|----------------|----------------|
| **Pipeline Video** | **Option A (CT32)** — CPU palette lookup (LUT), textura CT32 1.2 MB/frame, **sin GS CLUT**, sin swizzle CSM1/CSM2. Igual que Fallout 1 CE. |
| **Paleta/CLUT** | LUT 256 entradas en EE RAM (`s_palette[256]`), orden CT32 GS (ABGR). Re-subida forzada tras MVE/modal (`ps2_video_palette_mark_dirty`). |
| **VRAM** | Double-buffered CT32 (2 × 1.2 MB) + staging buffers. Sin T8/CLUT. |
| **Sync DMA** | `FlushCache(0)` + `dmaKit_wait_fast()` antes de cada subida. |
| **Filesystem** | Overlay runtime `FALLOUT2_RT/` (COW). Lecturas `mass:`/`host:`/`cdrom:`; escrituras (SAV, AUTOMAP) redirigidas a overlay. |
| **Confinamiento Save** | `.SAV` estrictamente bajo `MAPS/` — remap arreglado (separador `MAPS/`). |
| **Eviction Scripts** | `programListFree()` + purga extents en `scriptLoadAll` evita fragmentación heap. |
| **Audio** | audsrv IRX + hilo mixer EE dedicado (streaming SPU2). |
| **Input** | DualShock 2 + pad virtual; cola eventos bombeada desde `SDL_RenderPresent`. |
| **Heap** | Engine 16 MB (`PS2_MEM_ENGINE_HEAP_MAX`). Art cache 8 MB (bloques móviles). Trim antes de cargas grandes. |

---

## 🐛 Issues Arreglados (F7)

| Problema | Solución |
|----------|----------|
| **Corrupción paleta (estática rosa/negro)** | Migración a pipeline CT32 (Option A). Lookup CPU → textura CT32. Sin CLUT GS, sin CSM1/CSM2. |
| **Race CLUT (EE escribe mientras DMA lee)** | Double-buffer VRAM CT32 + barrera DMA explícita (`dmaKit_wait_fast` + `FlushCache`). |
| **Paleta MVE no se restauraba** | `ps2_video_palette_mark_dirty()` tras fin de movie fuerza re-subida completa LUT. |
| **Crash Skilldex (TLB Miss → PC unaligned)** | Arreglada alineación struct / puntero función en despachador UI. |
| **Remap save sin separador** | `_GameMap2Slot` inserta `/` cuando hay prefijo `MAPS`. |
| **Warnings Heap OOM (125 KB)** | Trim art cache antes de carga mapa; política evicción en `scriptLoadAll`. |

---

## 🤖 AI-Assisted Development / Desarrollo Asistido por IA

### English
This port was developed with AI assistance across architecture, diagnostics, and optimization:

| Model | Role |
|-------|------|
| **Gemini** (via OpenCode) | Low-level architecture: EE/GS pipeline, DMA synchronization, VRAM allocation strategies, gsKit integration. |
| **DeepSeek** (via OpenCode) | PS2SDK diagnostics: heap analysis, IRX module loading, TLB exception tracing, `dmaKit`/`gsKit` behavior. |
| **Claude** (via OpenCode) | Smoke harness state machines, performance profiling, RDRAM budget allocation, DMA/CLUT race condition resolution. |
| **Nemotron 3 Ultra** (via OpenCode) | Documentation synthesis, code review, cross-platform validation. |

> All AI-generated code was reviewed, tested on hardware/emulator, and integrated manually. The port builds and runs on real PS2 (FMCB + OPL) and PCSX2 (Strict mode).

### Español
Este port se desarrolló con asistencia de IA en arquitectura, diagnóstico y optimización:

| Modelo | Rol |
|--------|-----|
| **Gemini** (vía OpenCode) | Arquitectura bajo nivel: pipeline EE/GS, sincronización DMA, estrategias VRAM, integración gsKit. |
| **DeepSeek** (vía OpenCode) | Diagnósticos PS2SDK: análisis heap, carga módulos IRX, trazado excepciones TLB, comportamiento `dmaKit`/`gsKit`. |
| **Claude** (vía OpenCode) | Máquinas de estado smoke harness, profiling rendimiento, presupuesto RDRAM, resolución race conditions DMA/CLUT. |
| **Nemotron 3 Ultra** (vía OpenCode) | Síntesis de documentación, revisión de código, validación cross-platform. |

> Todo código generado por IA fue revisado, probado en hardware/emulador e integrado manualmente. El port compila y corre en PS2 real (FMCB + OPL) y PCSX2 (modo Strict).

---

## 💝 Support & Donations / Apoya el Proyecto

Si este port te ayuda a revivir el Yermo en PS2, invítame un café:

| Plataforma | Enlace |
|------------|--------|
| ☕ **Cafecito** | <https://cafecito.app/nicolasalucin> |
| 💙 **Ko-fi** | <https://ko-fi.com/nicolasalucin> |

---

## 📂 Repositorio

- **Código:** <https://github.com/nicolasalucin/fallout2-ce-ps2>
- **Issues:** <https://github.com/nicolasalucin/fallout2-ce-ps2/issues>
- **Rama:** `ps2` (desarrollo principal)
- **Distribución:** `dist/FALLOUT2/` (ELF + SYSTEM.CNF + README_PS2.txt)

---

## 📜 Créditos

- **Original Game:** © 1998 Interplay / Black Isle Studios
- **Community Edition Engine:** Alexey Batalov + contributors
- **PS2 Port:** Nicolas Salucin (nicolasalucin)
- **AI Assistance:** Gemini / DeepSeek / Claude / Nemotron via OpenCode

---

## ⚖️ Legal

Este es un proyecto de **preservación mediante ingeniería inversa**. **No incluye assets originales**.

**Debes poseer una copia legal de Fallout 2** (GOG, Steam, CDs originales) para usar este port. Copia tus `MASTER.DAT`, `CRITTER.DAT` y `PATCH000.DAT` a `FALLOUT2/`.