#include "zinc/zinc.h"
#include "zmem.h"

typedef struct {
    ZTaskFn     func;
    void        *ctx;
} ZTask;

typedef struct {
    Allocator   *allocator;
    pthread_t   thread;
    usize       worker;
    ZPool       *pool;
} ZThread;

struct ZPool {
    Allocator       *allocator;
    vec(ZThread)    threads;
    usize           max_jobs;
    vec(ZTask)      tasks;
    usize           pending;
    bool            destroyed;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  all_done;
};

static inline int thread_count() {
#if defined(_WIN32)
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? (int)n : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

static void *pool_worker(void *arg) {
    ZThread *t = arg;
    ZPool *pool = t->pool;
    if (!t->allocator) t->allocator = getArenaAllocator();
    for (;;) {
        pthread_mutex_lock(&pool->mutex);

        while (vecempty(pool->tasks) && !pool->destroyed)
            pthread_cond_wait(&pool->not_empty, &pool->mutex);

        if (pool->destroyed && vecempty(pool->tasks)) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }

        ZTask task = vecpop(pool->tasks);
        pthread_mutex_unlock(&pool->mutex);

        ZTaskCtx task_context = (ZTaskCtx){
            .allocator = t->allocator, .ctx = task.ctx, .worker = t->worker
        };
        task.func(&task_context);

        pthread_mutex_lock(&pool->mutex);
        if (--pool->pending == 0) pthread_cond_broadcast(&pool->all_done);
        pthread_mutex_unlock(&pool->mutex);
    }
    return NULL;
}

ZPool *pool_create(Allocator *allocator, usize max_jobs) {
    ZPool *pool     = aalloc(allocator, sizeof(ZPool));
    if (max_jobs == 0) max_jobs = thread_count();
    *pool = (ZPool){
        .allocator  = heapAllocator,
        .max_jobs   = max_jobs
    };

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->all_done, NULL);
    pthread_cond_init(&pool->not_empty, NULL);

    vecreservewith(pool->threads, pool->max_jobs, allocator);

    usize started = 0;
    for (usize i = 0; i < pool->max_jobs; i++) {
        pool->threads[started] = (ZThread){
            .allocator  = NULL,
            .worker     = started,
            .pool       = pool
        };
        usize status = pthread_create(
            &pool->threads[started].thread, NULL,
            pool_worker, &pool->threads[started]
        );

        if (status != 0) {
            adestroy(pool->threads[started].allocator);
            break;
        }
        started++;
    }
    vecsetlen(pool->threads, started);

    return pool;
}

void pool_submit(ZPool *pool, ZTaskFn func, void *ctx) {
    pthread_mutex_lock(&pool->mutex);
    vecpushwith(pool->tasks, ((ZTask){
        .func = func, .ctx = ctx
    }), pool->allocator);
    pool->pending++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);
}

void pool_wait(ZPool *pool) {
    pthread_mutex_lock(&pool->mutex);
    while (pool->pending > 0) pthread_cond_wait(&pool->all_done, &pool->mutex);
    pthread_mutex_unlock(&pool->mutex);
}

void pool_destroy(ZPool *pool) {
    pthread_mutex_lock(&pool->mutex);
    pool->destroyed = true;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);
    for (usize i = 0; i < veclen(pool->threads); i++)
        pthread_join(pool->threads[i].thread, NULL);
    pthread_cond_destroy(&pool->all_done);
    pthread_cond_destroy(&pool->not_empty);
    pthread_mutex_destroy(&pool->mutex);
    vecfree(pool->tasks);

    for (usize i = 0; i < veclen(pool->threads); i++)
        if (pool->threads[i].allocator)
            adestroy(pool->threads[i].allocator);

    vecfree(pool->threads);
}

void pool_allocator_stats(ZPool *pool, usize *used, usize *size) {
    if (!pool || !used || !size) return;

    for (usize i = 0; i < veclen(pool->threads); i++) {
        Allocator *allocator = pool->threads[i].allocator;
        if (!allocator) continue;

        *used += arenaLength(allocator->ctx);
        *size += arenaSize(allocator->ctx);
    }
}
