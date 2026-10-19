#include "ps2_mutex.h"

#include <kernel.h>
#include <string.h>

void ps2_recursive_mutex_init(ps2_recursive_mutex_t* mutex)
{
    ee_sema_t sema;

    if (mutex->sema > 0) {
        /* Re-initialization after an audio engine restart: the kernel
         * semaphore is still valid, only the ownership bookkeeping resets. */
        mutex->owner = -1;
        mutex->depth = 0;
        return;
    }

    memset(&sema, 0, sizeof(sema));
    sema.init_count = 1;
    sema.max_count = 1;

    mutex->sema = CreateSema(&sema);
    mutex->owner = -1;
    mutex->depth = 0;
}

void ps2_recursive_mutex_lock(ps2_recursive_mutex_t* mutex)
{
    int threadId;

    if (mutex->sema <= 0) {
        /* Initialization failed: fail open instead of hanging the engine. */
        return;
    }

    threadId = GetThreadId();

    if (mutex->owner == threadId) {
        mutex->depth++;
        return;
    }

    WaitSema(mutex->sema);
    mutex->owner = threadId;
    mutex->depth = 1;
}

void ps2_recursive_mutex_unlock(ps2_recursive_mutex_t* mutex)
{
    if (mutex->sema <= 0) {
        return;
    }

    /* Only the owner may unlock; the audio engine never unlocks a mutex it
     * does not hold, so a foreign unlock is a bug worth ignoring safely. */
    if (mutex->owner != GetThreadId()) {
        return;
    }

    mutex->depth--;
    if (mutex->depth <= 0) {
        mutex->owner = -1;
        SignalSema(mutex->sema);
    }
}
