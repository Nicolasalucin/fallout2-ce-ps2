/*
 * EE audio output for the Fallout PlayStation 2 port.
 *
 * The engine mixes 16-bit stereo PCM through its SDL audio device callback
 * (see src/audio_engine.cc). This layer runs that callback on a dedicated
 * EE thread and streams the result to the SPU2 through audsrv, so playback
 * advances in real time even while the engine's main thread is busy (the
 * MVE movie player spins against the sound cursor to keep A/V in sync).
 *
 * audsrv.irx (ps2sdk) is embedded into the ELF at build time, so players
 * need no extra files next to their game data. If any step fails, the SDL
 * layer reports "no audio device" and the engine disables sound instead.
 */

#ifndef PS2_AUDIO_H
#define PS2_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ps2_audio_callback_t)(void* userdata, unsigned char* stream, int len);

/* Opens the SPU2 stream (loads rom0:LIBSD + the embedded audsrv.irx), starts
 * the mixer thread (initially paused) and reports the actual output rate in
 * out_freq. Returns 0 on success, -1 if audio output is unavailable. */
int ps2_audio_open(int desired_freq, int samples, ps2_audio_callback_t callback, void* userdata, int* out_freq);

/* Must be called from the engine (main) thread every frame; streams finished
 * chunks to audsrv. All SIF RPCs stay on the engine thread by design. */
void ps2_audio_pump(void);

/* Pauses/resumes streaming (pausing also mutes the SPU2 ring). */
void ps2_audio_pause(int paused);

/* Stops the worker and releases audsrv (the engine calls this at exit). */
void ps2_audio_close(void);

/* Non-zero when the mixer is running in silent fallback mode (audsrv could
 * not be loaded or initialized). Diagnostics only. */
int ps2_audio_is_dummy(void);

#ifdef __cplusplus
}
#endif

#endif /* PS2_AUDIO_H */
