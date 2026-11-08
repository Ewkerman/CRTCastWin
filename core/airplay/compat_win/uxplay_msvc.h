/* CRTCast for Windows: force-included (/FI) into the UxPlay sources when building with MSVC,
 * so UxPlay itself stays unchanged. Adds the few POSIX pieces MSVC's C runtime lacks. */
#pragma once
#include <time.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#ifdef __cplusplus
extern "C" {
#endif
/* Only CLOCK_REALTIME is used (deadlines for pthread_cond_timedwait), which is what timespec_get gives. */
static __inline int crtcast_clock_gettime(int clock_id, struct timespec *ts) {
    (void) clock_id;
    return timespec_get(ts, TIME_UTC) ? 0 : -1;
}
#ifdef __cplusplus
}
#endif
#define clock_gettime crtcast_clock_gettime

#include <BaseTsd.h>
#ifndef _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif

/* UxPlay's threads.h, adapted for pthreads4w. UxPlay treats thread handles as plain values
 * (assigns 0, tests !handle, copies them), but pthreads4w's pthread_t is a struct. So UxPlay's handle is
 * a pointer to a heap-allocated pthread_t: 0/NULL means "no thread", and it is freed after the join.
 * Defining THREADS_H here turns UxPlay's own threads.h (same include guard) into a no-op. */
#ifndef THREADS_H
#define THREADS_H
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "unistd.h"

#define sleepms(x) usleep((x)*1000)

typedef pthread_t *thread_handle_t;

#define THREAD_RETVAL void *
#define THREAD_CREATE(handle, func, arg) 	do { 		(handle) = (pthread_t *) malloc(sizeof(pthread_t)); 		if ((handle) && pthread_create((handle), NULL, func, arg)) { free(handle); (handle) = NULL; } 	} while (0)
#define THREAD_JOIN(handle) do { pthread_join(*(handle), NULL); free(handle); } while (0)

typedef pthread_mutex_t mutex_handle_t;
typedef pthread_cond_t cond_handle_t;

#define MUTEX_CREATE(handle) pthread_mutex_init(&(handle), NULL)
#define MUTEX_LOCK(handle) pthread_mutex_lock(&(handle))
#define MUTEX_UNLOCK(handle) pthread_mutex_unlock(&(handle))
#define MUTEX_DESTROY(handle) pthread_mutex_destroy(&(handle))

#define COND_CREATE(handle) pthread_cond_init(&(handle), NULL)
#define COND_SIGNAL(handle) pthread_cond_signal(&(handle))
#define COND_DESTROY(handle) pthread_cond_destroy(&(handle))
#endif

/* Linux name for the TCP socket-option level (keepalive settings in raop_rtp_mirror.c). */
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif
