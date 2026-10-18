/*
 * audsrv-backed PCM output + worker thread for the Fallout PS2 port.
 *
 * Design notes:
 *
 *   - LIBSD comes from the ROM ("rom0:LIBSD"). audsrv.irx is located two
 *     ways, tried in this order:
 *
 *       1. As a file: "host:audsrv.irx" (the build copies it next to the
 *          ELF; PCSX2 uses the ELF's directory as the "host:" root), then
 *          the game data roots of real hardware. This is the only path
 *          that works under PCSX2: its IOP HLE breaks the raw EE->IOP DMA
 *          transfer used by SifExecModuleBuffer, which then silently
 *          "succeeds" with a module that never runs.
 *       2. The copy embedded into the ELF by the build (see CMakeLists.txt)
 *          executed via SifExecModuleBuffer; this is the normal,
 *          self-contained path on a real PS2.
 *
 *     Every attempt is verified with a bounded bind against the audsrv RPC
 *     server, so a broken path degrades to silent play instead of hanging
 *     forever inside audsrv_init().
 *
 *   - The engine mixes audio inside its SDL device callback
 *     (audioEngineMixin in src/audio_engine.cc): it pulls source frames from
 *     each playing sound buffer, converts them through SDL_AudioStream and
 *     mixes into the output block. Crucially, the sound buffer read cursor
 *     only advances while that callback runs, and the MVE movie player spins
 *     on that cursor to pace video. The callback therefore has to run in
 *     real time on its own EE thread, which is what this file provides.
 *
 *   - The mixer thread never issues SIF RPCs. Every audsrv call happens on
 *     the engine (main) thread through ps2_audio_pump(), fed from a small
 *     chunk ring that the mixer thread publishes into. This is deliberate:
 *     with the mixer itself calling audsrv, its RPCs raced the engine
 *     thread's file reads (the movie's .dat streaming) and the emulated SIF
 *     engine wedged solid the moment the intro movie started -- one thread
 *     packet-allocating and DMA-chaining while the other waited on the
 *     reply. Keeping every RPC on one thread also matches how console-era
 *     midware used the SIF layer, and it eliminates the whole class of
 *     cross-thread transport races on real hardware too.
 *
 *   - The mixer runs at the same priority as the engine thread: the movie
 *     player busy-waits, so only same-priority timeslicing lets the worker
 *     keep the engine's sound cursor advancing (a lower priority would
 *     deadlock the intro movies).
 *
 *   - The mix is paced by wall clock (DelayThread) and never touches a
 *     blocking audsrv call: audsrv_wait_audio() parks the IOP's single RPC
 *     thread inside the driver until the SPU2 drains, so one stalled ring
 *     would strand the EE caller forever and, through the movie player's
 *     cursor busy-wait, freeze the whole game. Instead the ring's free
 *     space (audsrv_available, computed purely on the IOP) gates a
 *     truncating audsrv_play_audio() call: a stalled pipeline degrades to
 *     silence (mixed chunks are dropped when the ring is full) while the
 *     game keeps running at the correct speed.
 *
 *   - Pause mutes the SPU2 ring via audsrv_stop_audio from the engine
 *     thread and parks the mixer on its semaphore; resume signals it.
 *
 *   - If anything fails the caller treats it as "machine with no sound card":
 *     audioEngineInit() returns false and the engine runs silent instead of
 *     hanging on the (never advancing) sound cursor.
 */

#include "ps2_audio.h"
#include "ps2_storage.h"
#include "ps2_time.h"

#include <stdio.h>
#include <string.h>

#include <audsrv.h>
#include <delaythread.h>
#include <fcntl.h>
#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <unistd.h>

/* Generated at build time from ps2sdk's audsrv.irx (see CMakeLists.txt). */
extern const unsigned char g_audsrv_irx[];
extern const unsigned int g_audsrv_irx_size;

/* Small-data base pointer owned by the ps2sdk crt0; worker threads must
 * inherit it or small-data references crash. */
extern void* _gp;

#define PS2_AUDIO_BITS 16
#define PS2_AUDIO_CHANNELS 2
#define PS2_AUDIO_BYTES_PER_FRAME ((PS2_AUDIO_BITS / 8) * PS2_AUDIO_CHANNELS)
#define PS2_AUDIO_MAX_SAMPLES 2048
#define PS2_AUDIO_STACK_SIZE (16 * 1024)

static ps2_audio_callback_t s_callback;
static void* s_userdata;

static int s_freq;
static int s_chunk_bytes;
static int s_chunk_us;
static int s_thread_id = -1;
static int s_sema = -1;

static volatile int s_running;
static volatile int s_paused = 1;
static volatile int s_muted;

/* 64-byte aligned so DMA-bound copies stay fast; S16 stereo. */
static unsigned char s_chunk_buffer[PS2_AUDIO_MAX_SAMPLES * PS2_AUDIO_BYTES_PER_FRAME] __attribute__((aligned(64)));
static unsigned char s_thread_stack[PS2_AUDIO_STACK_SIZE] __attribute__((aligned(16)));

/* Finished chunks travel from the mixer thread to the engine thread here.
 * Small on purpose: if the pump is late the oldest audio is already stale,
 * so only ~half a second of jitter absorption is useful. */
#define PS2_AUDIO_RING_CHUNKS 4
static unsigned char s_ring[PS2_AUDIO_RING_CHUNKS][PS2_AUDIO_MAX_SAMPLES * PS2_AUDIO_BYTES_PER_FRAME] __attribute__((aligned(64)));
static volatile int s_ring_head; /* next chunk the pump sends */
static volatile int s_ring_tail; /* next chunk the mixer publishes */

static void audio_worker(void* arg)
{
    uint64_t deadline_us = 0;
    int scheduled = 0;

    (void)arg;

    while (s_running) {
        if (s_paused || s_callback == NULL) {
            /* A semaphore instead of SleepThread/WakeupThread on purpose:
             * semaphore signals are sticky, so an unpause racing with this
             * check can never leave the mixer asleep forever (the movie
             * player busy-waits on the sound cursor, so a lost wakeup means
             * a black screen). */
            WaitSema(s_sema);
            scheduled = 0;
            continue;
        }

        /* The engine's movie player busy-waits on the cursor advanced by
         * this callback, so mixing must never stop. Production is paced by
         * wall clock through an absolute deadline: sleeping `s_chunk_us`
         * *after* each mix adds the callback's own cost (and DelayThread
         * jitter) to every period, running the mixer a few percent below
         * real time; the pipeline then drains slowly and streamed audio
         * (movie narration) comes out chopped. A full ring still drops the
         * chunk: the movie keeps its timing, silently. */
        if (scheduled) {
            uint64_t now_us = ps2_time_us();

            if (now_us < deadline_us) {
                DelayThread((int)(deadline_us - now_us));
            } else if (now_us - deadline_us > (uint64_t)s_chunk_us * 4) {
                /* Stalled for several chunks (resume, disk hitch): resync
                 * the schedule instead of bursting stale audio. */
                scheduled = 0;
            }
        }

        memset(s_chunk_buffer, 0, (size_t)s_chunk_bytes);
        s_callback(s_userdata, s_chunk_buffer, s_chunk_bytes);

        if (s_ring_tail - s_ring_head < PS2_AUDIO_RING_CHUNKS) {
            memcpy(s_ring[s_ring_tail % PS2_AUDIO_RING_CHUNKS], s_chunk_buffer, (size_t)s_chunk_bytes);
            __asm__ volatile("" ::: "memory");
            s_ring_tail++;
        }

        if (!scheduled) {
            deadline_us = ps2_time_us();
            scheduled = 1;
        }
        deadline_us += s_chunk_us;
    }

    ExitDeleteThread();
}

void ps2_audio_pump(void)
{
    static uint32_t s_last_pump_ms;
    uint32_t now_ms;
    int fed = 0;

    /* Engine (main) thread only: see the ring note above. */
    if (s_thread_id < 0 || s_paused) {
        return;
    }

    /* Besides the per-frame calls this also runs from the movie player's
     * millisecond wait loops, so clamp the audsrv RPC rate (frame presents
     * are >= 16 ms apart, so those calls always go through). */
    now_ms = ps2_time_ms();
    if (s_last_pump_ms != 0 && (uint32_t)(now_ms - s_last_pump_ms) < 8) {
        return;
    }
    s_last_pump_ms = now_ms;

    s_muted = 0;

    /* Feed as much as the SPU2 ring has room for; audsrv_play_audio copies
     * into the IOP ring and truncates, it never blocks. */
    while (fed < PS2_AUDIO_RING_CHUNKS && s_ring_head != s_ring_tail) {
        if (audsrv_available() < s_chunk_bytes) {
            break;
        }

        audsrv_play_audio((const char*)s_ring[s_ring_head % PS2_AUDIO_RING_CHUNKS], s_chunk_bytes);

        __asm__ volatile("" ::: "memory");
        s_ring_head++;
        fed++;
    }
}

/* audsrv's RPC server runs on the IOP; a successful bind means the module
 * actually started and its server thread registered, i.e. audsrv_init() can
 * no longer hang waiting for it. wait_ms <= 0 means "check once".
 *
 * The bind is issued once, non-blocking (SIF_RPC_M_NOWAIT), and only its
 * reply is polled with a bounded wait. A blocking sceSifBindRpc() parks the
 * thread on a semaphore signalled by the SIF completion handler; if a
 * completion is ever lost - which happens on PCSX2 under heavy I/O; see the
 * file header - that wait never returns and the boot hangs here. Polling
 * turns a lost completion into "audio unavailable" instead.
 *
 * The client data is static because the IOP's reply carries a pointer to it,
 * so it must stay valid until the completion is processed. A new bind may
 * only be sent once the previous one completed; the completion releases the
 * packet (hdr.pkt_addr back to NULL), which is also how "bound, no server
 * was listening" is told apart from "still in flight". */
static SifRpcClientData_t s_bind_cd;
static int s_bind_inflight;

static void audsrv_bind_start(void)
{
    if (s_bind_cd.server != NULL) {
        return;
    }

    if (s_bind_inflight) {
        if (s_bind_cd.hdr.pkt_addr != NULL) {
            return;
        }
        s_bind_inflight = 0;
    }

    memset(&s_bind_cd, 0, sizeof(s_bind_cd));
    if (sceSifBindRpc(&s_bind_cd, AUDSRV_IRX, SIF_RPC_M_NOWAIT) >= 0) {
        s_bind_inflight = (s_bind_cd.hdr.pkt_addr != NULL);
    }
}

static int audsrv_server_is_up(int wait_ms)
{
    unsigned int start = ps2_time_ms();

    for (;;) {
        if (s_bind_cd.server != NULL) {
            return 1;
        }

        audsrv_bind_start();

        if (wait_ms <= 0 || (int)(ps2_time_ms() - start) >= wait_ms) {
            return 0;
        }

        /* The reply is processed from the SIF command interrupt, so sleeping
         * is enough to let it land; a spinning thread would starve it. */
        DelayThread(1000);
    }
}

int ps2_audio_open(int desired_freq, int samples, ps2_audio_callback_t callback, void* userdata, int* out_freq)
{
    int ret;
    int module_result = -1;

    if (s_thread_id >= 0) {
        /* Already streaming: just swap the callback sink. */
        s_callback = callback;
        s_userdata = userdata;
        if (out_freq != NULL) {
            *out_freq = s_freq;
        }
        return 0;
    }

    if (desired_freq <= 0) {
        desired_freq = 22050;
    }
    if (samples <= 0) {
        samples = 1024;
    }
    if (samples > PS2_AUDIO_MAX_SAMPLES) {
        samples = PS2_AUDIO_MAX_SAMPLES;
    }
    s_chunk_bytes = samples * PS2_AUDIO_BYTES_PER_FRAME;

    /* Note: audsrv's bring-up used to be skipped when the game ran from the
     * host: filesystem, because on PCSX2 the first RPC bind after loading
     * LIBSD appeared to hang inside sceSifBindRpc(). The real cause was the
     * boot-time dopen()/readdir() storm, which corrupted the IOP-HLE's RPC
     * bookkeeping and left packets that never completed; the directory cache
     * in fs.c removed it. Emulator runs use the same path as a real console
     * now, and every step below still degrades gracefully (bounded polls)
     * when a module cannot be started. */

    sceSifInitRpc(0);

    /* SDR driver for audsrv; loading it twice is harmless. If the load
     * fails but LIBSD is already resident, audsrv still works, so this is
     * not treated as fatal: audsrv_init() is the real test. */
    ret = SifLoadModule("rom0:LIBSD", 0, NULL);
    printf("[ps2] audio: rom0:LIBSD -> %d\n", ret);

    if (audsrv_server_is_up(0)) {
        printf("[ps2] audio: audsrv server already present\n");
    } else {
        /* File-based loads first (PCSX2-friendly; see file header). */
        static const char* const paths[] = {
            "host:audsrv.irx",
            "host:local_assets/audsrv.irx",
            "mass:/FALLOUT2/audsrv.irx",
            "hdd0:/FALLOUT2/audsrv.irx",
        };
        const int path_count = (int)(sizeof(paths) / sizeof(paths[0]));
        int i;

        for (i = 0; i < path_count && !audsrv_server_is_up(0); i++) {
            int id;

            printf("[ps2] audio: loading '%s'\n", paths[i]);
            id = SifLoadModule(paths[i], 0, NULL);

            printf("[ps2] audio: %s -> %d\n", paths[i], id);
            if (id >= 0 && !audsrv_server_is_up(1500)) {
                /* Loaded but its RPC server never appeared: unload it so
                 * the next attempt can reuse the module name. */
                SifUnloadModule(id);
            }
        }
    }

    /* Embedded module: the normal, file-less path on a real PS2. */
    if (!audsrv_server_is_up(0)) {
        ret = SifExecModuleBuffer((void*)g_audsrv_irx, g_audsrv_irx_size, 0, NULL, &module_result);
        printf("[ps2] audio: audsrv.irx (embedded) -> %d (result %d)\n", ret, module_result);
    }

    if (!audsrv_server_is_up(2000)) {
        printf("[ps2] audio: no audsrv RPC server, continuing without sound\n");
        return -1;
    }

    ret = audsrv_init();
    printf("[ps2] audio: audsrv_init -> %d\n", ret);
    if (ret != AUDSRV_ERR_NOERROR) {
        return -1;
    }

    /* audsrv resamples to the SPU2 native rate internally ("convert it to
     * SPU2's native format if required"), but stay defensive about the
     * format: requested rate, its double (22050 -> 44100), then natives. */
    {
        int candidates[4];
        int count = 0;
        int i;

        candidates[count++] = desired_freq;
        candidates[count++] = desired_freq * 2;
        candidates[count++] = 48000;
        candidates[count++] = 44100;

        s_freq = 0;
        for (i = 0; i < count; i++) {
            audsrv_fmt_t fmt;

            fmt.freq = candidates[i];
            fmt.bits = PS2_AUDIO_BITS;
            fmt.channels = PS2_AUDIO_CHANNELS;

            if (audsrv_set_format(&fmt) == AUDSRV_ERR_NOERROR) {
                s_freq = candidates[i];
                break;
            }
        }

        printf("[ps2] audio: set_format -> %d Hz\n", s_freq);
        if (s_freq == 0) {
            audsrv_quit();
            return -1;
        }
    }

    /* Wall-clock duration of one mixed chunk at the configured rate: the
     * worker paces the callback with this so the engine's cursors advance
     * in real time even if the output pipeline stalls. */
    s_chunk_us = (samples * 1000000) / s_freq;

    audsrv_set_volume(MAX_VOLUME);

    s_ring_head = 0;
    s_ring_tail = 0;
    s_callback = callback;
    s_userdata = userdata;
    s_paused = 1;
    s_running = 1;

    {
        ee_thread_status_t status;
        ee_thread_t thread;
        ee_sema_t sema;
        int priority = 32;

        /* Same priority as the engine thread (see file header). */
        if (ReferThreadStatus(GetThreadId(), &status) >= 0) {
            priority = status.current_priority;
        }

        memset(&sema, 0, sizeof(sema));
        sema.init_count = 0;
        sema.max_count = 1;
        s_sema = CreateSema(&sema);
        if (s_sema < 0) {
            printf("[ps2] audio: CreateSema -> %d\n", (int)s_sema);
            s_running = 0;
            audsrv_quit();
            return -1;
        }

        memset(&thread, 0, sizeof(thread));
        thread.func = (void*)audio_worker;
        thread.stack = s_thread_stack;
        thread.stack_size = sizeof(s_thread_stack);
        thread.gp_reg = &_gp;
        thread.initial_priority = priority;

        s_thread_id = CreateThread(&thread);
        printf("[ps2] audio: worker thread -> %d (prio %d)\n", s_thread_id, priority);
        if (s_thread_id < 0) {
            s_running = 0;
            audsrv_quit();
            return -1;
        }

        if (StartThread(s_thread_id, NULL) < 0) {
            DeleteThread(s_thread_id);
            s_thread_id = -1;
            s_running = 0;
            audsrv_quit();
            return -1;
        }
    }

    if (out_freq != NULL) {
        *out_freq = s_freq;
    }

    return 0;
}

void ps2_audio_pause(int paused)
{
    if (s_thread_id < 0) {
        return;
    }

    if (paused != 0) {
        s_paused = 1;

        /* Mute whatever is still queued in the SPU2 ring. This is a SIF
         * call, so it runs here on the engine thread (see the ring note). */
        if (!s_muted) {
            s_muted = 1;
            audsrv_stop_audio();
        }
    } else if (s_paused) {
        s_paused = 0;
        SignalSema(s_sema);
    }
}

void ps2_audio_close(void)
{
    int i;

    if (s_thread_id < 0) {
        return;
    }

    s_running = 0;
    s_paused = 0;
    SignalSema(s_sema);

    /* Give the worker a moment to leave its loop (one chunk is ~46 ms at
     * 22050 Hz) before tearing audsrv down. */
    for (i = 0; i < 200; i++) {
        ee_thread_status_t status;

        if (ReferThreadStatus(s_thread_id, &status) < 0 || status.status == THS_DORMANT) {
            break;
        }

        DelayThread(1000);
    }

    s_thread_id = -1;
    s_callback = NULL;
    s_userdata = NULL;

    if (s_sema >= 0) {
        DeleteSema(s_sema);
        s_sema = -1;
    }

    audsrv_stop_audio();
    audsrv_quit();
}
