#ifndef PS2_MUTEX_H_
#define PS2_MUTEX_H_

/*
 * Blocking, recursion-capable mutex for the PS2 port.
 *
 * libstdc++ on the ps2dev toolchain is configured for weak POSIX threads and
 * ps2sdk ships no pthread library, so __gthread_active_p() is false and every
 * std::mutex / std::recursive_mutex operation compiles to a no-op (see
 * bits/gthr-posix.h: __gthread_mutex_lock() returns 0 without locking).
 *
 * The audio engine depends on those locks: the mixer runs on its own EE thread
 * (see platform/ps2/audio_audsrv.c) and the per-sound-buffer mutex is what
 * keeps it away from a buffer while the engine thread fills it in
 * (_addSoundData) or releases it (audioEngineSoundBufferRelease). With no-op
 * locks the mixer can read a released buffer's NULL data pointer and its
 * already-freed conversion stream, corrupting memory behind the audio path
 * (the dev memcpy guard caught exactly that on PCSX2).
 *
 * This mutex is built on EE kernel semaphores. Initialization is explicit:
 * the audio engine calls init() from audioEngineInit(), which runs before the
 * mixer thread exists, so no kernel object is created from a static
 * constructor.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps2_recursive_mutex {
    /* EE semaphore id; 0 until ps2_recursive_mutex_init() succeeds. */
    int sema;

    /* Thread id currently holding the mutex, or -1 when free. */
    int owner;

    /* Recursion depth for the owning thread. */
    int depth;
} ps2_recursive_mutex_t;

void ps2_recursive_mutex_init(ps2_recursive_mutex_t* mutex);
void ps2_recursive_mutex_lock(ps2_recursive_mutex_t* mutex);
void ps2_recursive_mutex_unlock(ps2_recursive_mutex_t* mutex);

#ifdef __cplusplus
}

/* RAII-friendly wrapper: the existing std::lock_guard<std::...> call sites
 * work unchanged once their type parameter is AudioEngineMutex. */
class Ps2RecursiveMutex {
public:
    /* Called by audioEngineInit() before the mixer thread starts. */
    void init() { ps2_recursive_mutex_init(&m_mutex); }

    void lock() { ps2_recursive_mutex_lock(&m_mutex); }
    void unlock() { ps2_recursive_mutex_unlock(&m_mutex); }

private:
    ps2_recursive_mutex_t m_mutex;
};
#endif

#endif /* PS2_MUTEX_H_ */
