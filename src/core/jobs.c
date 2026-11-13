/* Threads, mutexes, semaphores and a parallel-for job system. */
#include "../lx_internal.h"

#if LX_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
struct LxThread { HANDLE h; int (*fn)(void *); void *arg; };
struct LxMutex { CRITICAL_SECTION cs; };
struct LxSemaphore { HANDLE h; };
static unsigned __stdcall thread_tramp(void *p) { LxThread *t = p; return (unsigned)t->fn(t->arg); }
LxThread *lx_thread_create(int (*fn)(void *), void *arg) {
    LxThread *t = lx_calloc(1, sizeof *t);
    t->fn = fn; t->arg = arg;
    t->h = (HANDLE)_beginthreadex(NULL, 0, thread_tramp, t, 0, NULL);
    return t;
}
void lx_thread_join(LxThread *t) { WaitForSingleObject(t->h, INFINITE); CloseHandle(t->h); lx_free(t); }
LxMutex *lx_mutex_create(void) { LxMutex *m = lx_alloc(sizeof *m); InitializeCriticalSection(&m->cs); return m; }
void lx_mutex_destroy(LxMutex *m) { DeleteCriticalSection(&m->cs); lx_free(m); }
void lx_mutex_lock(LxMutex *m) { EnterCriticalSection(&m->cs); }
void lx_mutex_unlock(LxMutex *m) { LeaveCriticalSection(&m->cs); }
LxSemaphore *lx_sem_create(int initial) { LxSemaphore *s = lx_alloc(sizeof *s); s->h = CreateSemaphoreW(NULL, initial, 0x7fffffff, NULL); return s; }
void lx_sem_destroy(LxSemaphore *s) { CloseHandle(s->h); lx_free(s); }
void lx_sem_post(LxSemaphore *s, int count) { ReleaseSemaphore(s->h, count, NULL); }
void lx_sem_wait(LxSemaphore *s) { WaitForSingleObject(s->h, INFINITE); }
int lx_cpu_count(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
#else
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
struct LxThread { pthread_t t; int (*fn)(void *); void *arg; };
struct LxMutex { pthread_mutex_t m; };
struct LxSemaphore { sem_t s; };
static void *thread_tramp(void *p) { LxThread *t = p; t->fn(t->arg); return NULL; }
LxThread *lx_thread_create(int (*fn)(void *), void *arg) {
    LxThread *t = lx_calloc(1, sizeof *t);
    t->fn = fn; t->arg = arg;
    pthread_create(&t->t, NULL, thread_tramp, t);
    return t;
}
void lx_thread_join(LxThread *t) { pthread_join(t->t, NULL); lx_free(t); }
LxMutex *lx_mutex_create(void) { LxMutex *m = lx_alloc(sizeof *m); pthread_mutex_init(&m->m, NULL); return m; }
void lx_mutex_destroy(LxMutex *m) { pthread_mutex_destroy(&m->m); lx_free(m); }
void lx_mutex_lock(LxMutex *m) { pthread_mutex_lock(&m->m); }
void lx_mutex_unlock(LxMutex *m) { pthread_mutex_unlock(&m->m); }
LxSemaphore *lx_sem_create(int initial) { LxSemaphore *s = lx_alloc(sizeof *s); sem_init(&s->s, 0, (unsigned)initial); return s; }
void lx_sem_destroy(LxSemaphore *s) { sem_destroy(&s->s); lx_free(s); }
void lx_sem_post(LxSemaphore *s, int count) { for (int i = 0; i < count; i++) sem_post(&s->s); }
void lx_sem_wait(LxSemaphore *s) { while (sem_wait(&s->s) != 0) {} }
int lx_cpu_count(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif

/* ---------------- job system ----------------
 * A parallel_for publishes one "batch": workers and the caller grab chunks
 * with an atomic counter until the range is exhausted. No allocations per call. */
#define LX_MAX_WORKERS 31

typedef struct JobBatch {
    LxJobFn fn;
    void *user;
    int count, grain;
    volatile long next;
    volatile long remaining_workers;
} JobBatch;

static struct {
    LxThread *threads[LX_MAX_WORKERS];
    int worker_count;
    LxSemaphore *wake, *done;
    LxMutex *submit;
    JobBatch *batch;
    volatile long quit;
    bool initialised;
} g_jobs;

static void run_chunks(JobBatch *b, int thread_index) {
    for (;;) {
        long start = lx_atomic_add(&b->next, b->grain) - b->grain;
        if (start >= b->count) break;
        int end = (int)start + b->grain;
        if (end > b->count) end = b->count;
        b->fn(b->user, (int)start, end, thread_index);
    }
}

typedef struct { int index; } WorkerArg;
static WorkerArg g_worker_args[LX_MAX_WORKERS];

static int worker_main(void *p) {
    int index = ((WorkerArg *)p)->index;
    for (;;) {
        lx_sem_wait(g_jobs.wake);
        if (lx_atomic_load(&g_jobs.quit)) break;
        JobBatch *b = g_jobs.batch;
        run_chunks(b, index);
        if (lx_atomic_add(&b->remaining_workers, -1) == 0) lx_sem_post(g_jobs.done, 1);
    }
    return 0;
}

void lx_jobs_init(int threads) {
    if (g_jobs.initialised) return;
    if (threads <= 0) threads = lx_cpu_count() - 1;
    if (threads > LX_MAX_WORKERS) threads = LX_MAX_WORKERS;
    if (threads < 0) threads = 0;
    g_jobs.wake = lx_sem_create(0);
    g_jobs.done = lx_sem_create(0);
    g_jobs.submit = lx_mutex_create();
    g_jobs.worker_count = threads;
    for (int i = 0; i < threads; i++) {
        g_worker_args[i].index = i + 1;
        g_jobs.threads[i] = lx_thread_create(worker_main, &g_worker_args[i]);
    }
    g_jobs.initialised = true;
    LX_INFO("Job system: %d worker threads", threads);
}

void lx_jobs_shutdown(void) {
    if (!g_jobs.initialised) return;
    lx_atomic_store(&g_jobs.quit, 1);
    lx_sem_post(g_jobs.wake, g_jobs.worker_count);
    for (int i = 0; i < g_jobs.worker_count; i++) lx_thread_join(g_jobs.threads[i]);
    lx_sem_destroy(g_jobs.wake);
    lx_sem_destroy(g_jobs.done);
    lx_mutex_destroy(g_jobs.submit);
    memset(&g_jobs, 0, sizeof g_jobs);
}

int lx_jobs_thread_count(void) { return g_jobs.worker_count + 1; }

void lx_parallel_for(int count, int grain, LxJobFn fn, void *user) {
    if (count <= 0) return;
    if (grain < 1) grain = 1;
    if (!g_jobs.initialised || g_jobs.worker_count == 0 || count <= grain) {
        fn(user, 0, count, 0);
        return;
    }
    lx_mutex_lock(g_jobs.submit);
    JobBatch b;
    b.fn = fn; b.user = user; b.count = count; b.grain = grain;
    b.next = 0;
    int workers = g_jobs.worker_count;
    int chunks = (count + grain - 1) / grain;
    if (workers > chunks - 1) workers = chunks - 1;
    b.remaining_workers = workers;
    g_jobs.batch = &b;
    if (workers > 0) lx_sem_post(g_jobs.wake, workers);
    run_chunks(&b, 0);
    if (workers > 0) lx_sem_wait(g_jobs.done);
    g_jobs.batch = NULL;
    lx_mutex_unlock(g_jobs.submit);
}
