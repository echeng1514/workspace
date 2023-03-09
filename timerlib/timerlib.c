
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* time to wait the init of ticker thread in timer_init() function */
#define INIT_WAIT_TIME 5

/* for conversion purpose */
#define NANO_PER_MICRO 1000
#define MICRO_PER_SEC 1000000

/* max number of microsecond in a timeval struct */
#define MAX_USEC 999999

/* microseconds in a millisecond */
#define ALMOST_NOW 1000

#define TIMER_KIND ITIMER_REAL
#define SIG_TO_WAIT SIGALRM
/*
 * evals true if struct timeval t1 defines a time strictly before t2;
 * false otherwise.
 */
#define TV_LESS_THAN(t1, t2)                                                                                           \
    (((t1).tv_sec < (t2).tv_sec) || \
    (((t1).tv_sec == (t2).tv_sec) && ((t1).tv_usec < (t2).tv_usec)))

/*
 * Assign struct timeval tgt the time interval between the absolute
 * times t1 and t2.
 * IT IS ASSUMED THAT t1 > t2, use TV_LESS_THAN macro to test it.
 */
#define TV_MINUS(t1, t2, tgt)                                                                                          \
    if ((t1).tv_usec >= (t2).tv_usec) {                                                                                \
        (tgt).tv_sec  = (t1).tv_sec - (t2).tv_sec;                                                                     \
        (tgt).tv_usec = (t1).tv_usec - (t2).tv_usec;                                                                   \
    }                                                                                                                  \
    else {                                                                                                             \
        (tgt).tv_sec  = (t1).tv_sec - (t2).tv_sec - 1;                                                                 \
        (tgt).tv_usec = (t1).tv_usec + (MAX_USEC - (t2).tv_usec);                                                      \
    }

typedef struct timer
{
    struct timeval timeout;
    void (*handler)(void*);
    void*         handler_arg;
    int           timerId;
    int           in_use;
    int           cancelled;
    struct timer* next;
    struct timer* prev;
} Timer;

typedef struct timerqueue
{
    Timer*          first;
    Timer*          last;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    pthread_t       tid;
    uint32_t        cur_id;
} TimerQueue;

static TimerQueue timerq;
void TimerPrint();
static void TimerFree(Timer* t)
{
    if (t) {
        free(t);
        t = NULL;
    }

    return;
}

static void TimerDequeue(Timer* t)
{
    if (NULL == t) {
        return;
    }

    if (t->prev == NULL) {
        timerq.first = t->next;
    }
    else {
        t->prev->next = t->next;
    }

    if (t->next == NULL) {
        timerq.last = t->prev;
    }
    else {
        t->next->prev = t->prev;
    }

    TimerFree(t);
    return;
}

static void TimerStart(struct timeval* abs_to)
{
    struct itimerval relative = {{0, 0}, {0, 0}};
    struct timeval   abs_now;
    int              rv;

    if (abs_to == NULL) {
        return;
    }

    /* absolute to relative time */
    gettimeofday(&abs_now, NULL);

    if (TV_LESS_THAN(abs_now, *abs_to)) {
        /* ok, timeout is in the future */
        TV_MINUS(*abs_to, abs_now, relative.it_value);
    }
    else {
        /*
         * ouch, timeout is in the past! Let's set it
         * to a very near future value.
         */
        relative.it_value.tv_sec  = 0;
        relative.it_value.tv_usec = ALMOST_NOW;
    }

    rv = setitimer(TIMER_KIND, &relative, NULL);

    if (rv == -1) {
        /* This should never happen but I dont trust myself */
        perror("setitimer");
        exit(1);
    }

    return;
}

static void* cronometer(void* arg)
{
    sigset_t mask;
    int      sig;
    void (*handle)(void*);
    void* handleArg;

    sigemptyset(&mask);
    sigaddset(&mask, SIGALRM);

    /*
     * Set this thread to be cancelled only in the cancellation
     * points (pthread_cond_wait() and sigwait()).
     */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    pthread_mutex_lock(&timerq.mutex);
    pthread_cond_signal(&timerq.cond);
    pthread_mutex_unlock(&timerq.mutex);
    while (1) {
        pthread_mutex_lock(&timerq.mutex);
        while (timerq.first == NULL) {
            pthread_cond_wait(&timerq.cond, &timerq.mutex);
        }

        TimerStart(&(timerq.first->timeout));
        timerq.first->in_use = 1;
        pthread_mutex_unlock(&timerq.mutex);

        /* wait for a pending SIGALRM */
        sigwait(&mask, &sig);
        pthread_mutex_lock(&timerq.mutex);

        if (timerq.first->cancelled == 1) {
            TimerDequeue(timerq.first);
            pthread_mutex_unlock(&timerq.mutex);
            continue;
        }

        /*
         * we cannot be cancelled while freeing memory and executing
         * an handler.
         */
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        handle    = timerq.first->handler;
        handleArg = timerq.first->handler_arg;
        TimerDequeue(timerq.first);
        /*
         * unlock the mutex before executing an handler to avoid
         * possible deadlock inside it.
         */
        pthread_mutex_unlock(&timerq.mutex);
        handle(handleArg);
        /* reset cancel state (cancel type has not  been changed) */
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    }

    return NULL;
}

int TimerInit(void)
{
    int             rv;
    struct timespec ts;
    struct timeval  tv;

    rv = pthread_mutex_init(&timerq.mutex, NULL);
    if (rv != 0) {
        return -1;
    }
    rv = pthread_cond_init(&timerq.cond, NULL);
    if (rv != 0) {
        pthread_mutex_destroy(&timerq.mutex);
        return -1;
    }

    timerq.first  = NULL;
    timerq.last   = NULL;
    timerq.cur_id = 0;
    pthread_mutex_lock(&timerq.mutex);
    rv = pthread_create(&timerq.tid, NULL, cronometer, NULL);
    if (rv != 0) {
        pthread_mutex_unlock(&timerq.mutex);
        pthread_mutex_destroy(&timerq.mutex);
        pthread_cond_destroy(&timerq.cond);
        return -1;
    }

    gettimeofday(&tv, NULL);
    ts.tv_sec  = tv.tv_sec + INIT_WAIT_TIME;
    ts.tv_nsec = tv.tv_usec * 1000;
    rv         = pthread_cond_timedwait(&timerq.cond, &timerq.mutex, &ts);

    if (rv != 0) {
        if (rv == ETIMEDOUT) {
            pthread_cancel(timerq.tid);
            pthread_join(timerq.tid, NULL);
        }
        pthread_mutex_unlock(&timerq.mutex);
        pthread_mutex_destroy(&timerq.mutex);
        pthread_cond_destroy(&timerq.cond);
        return -1;
    }

    pthread_detach(timerq.tid);
    pthread_mutex_unlock(&timerq.mutex);
    return 0;
}

int TimerAdd(long sec, long usec, void (*handle)(void*), void* handleArg, int* timerId)
{
    struct timeval new;
    Timer* tmp = NULL;
    Timer* app = NULL;
    int    id  = 0;

    if (NULL == handle) {
        return -1;
    }
    if ((sec < 0) || (usec < 0) || ((sec == 0) && (usec == 0))) {
        return -1;
    }

    pthread_mutex_lock(&timerq.mutex);
    app = (Timer*)malloc(sizeof(Timer));
    if (NULL == app) {
        pthread_mutex_unlock(&timerq.mutex);
        return -1;
    }

    gettimeofday(&new, NULL);
    new.tv_sec += sec + (new.tv_usec + usec) / MICRO_PER_SEC;
    // new.tv_usec += usec + (new.tv_usec + usec) / MICRO_PER_SEC;
    new.tv_usec = (new.tv_usec + usec) % MICRO_PER_SEC;

    memcpy(&app->timeout, &new, sizeof(struct timeval));
    id               = timerq.cur_id++;
    app->handler     = handle;
    app->handler_arg = handleArg;
    app->timerId     = id;
    app->in_use      = 1;
    app->cancelled   = 0;
    app->prev = app->next = NULL;

    if (NULL == timerq.first) {
        timerq.first = app;
        timerq.last  = app;
        *timerId     = id;
        printf("timerq.first[%ld %ld]\n",timerq.first->timeout.tv_sec, timerq.first->timeout.tv_usec);
        pthread_cond_signal(&timerq.cond);
        pthread_mutex_unlock(&timerq.mutex);
        return id;
    }

    tmp = timerq.first;
    while (tmp != NULL) {
        if (TV_LESS_THAN(app->timeout, tmp->timeout)) {
            break;
        }
        tmp = tmp->next;
    }

    if (tmp == NULL) {
        // app->prev         = timerq.last->next;
        /*修改BUG*/
        app->prev         = timerq.last;
        timerq.last->next = app;
        timerq.last       = app;
    }
    else if (tmp->prev == NULL) {
        tmp->prev    = app;
        app->next    = tmp;
        timerq.first = app;
    }
    else {
        app->prev       = tmp->prev;
        app->next       = tmp;
        tmp->prev->next = app;
        tmp->prev       = app;
    }
    pthread_mutex_unlock(&timerq.mutex);
    *timerId = id;
    return id;
}

void TimerRemove(int timerId, void (*free_arg)(void*))
{
    Timer* t;
    pthread_mutex_lock(&timerq.mutex);

    t = timerq.first;
    while (t != NULL) {
        if (t->timerId == timerId) {
            break;
        }
        t = t->next;
    }

    if (t == NULL) {
        pthread_mutex_unlock(&timerq.mutex);
        return;
    }

    if (t->in_use == 1) {
        if (free_arg) {
            free_arg(t->handler_arg);
            t->handler_arg = NULL;
        }
        t->cancelled = 1;
        pthread_mutex_unlock(&timerq.mutex);
        return;
    }

    if (free_arg) {
        free_arg(t->handler_arg);
    }
    TimerDequeue(t);
    pthread_mutex_unlock(&timerq.mutex);
    return;
}

void TimerDestroy()
{
    Timer* t = NULL;

    pthread_cancel(timerq.tid);
    pthread_join(timerq.tid, NULL);
  
    t = timerq.last;
    while (t != NULL) {
        timerq.last = t->prev;
        TimerFree(t);
        t = timerq.last;
    }

    pthread_mutex_destroy(&timerq.mutex);
    pthread_cond_destroy(&timerq.cond);
    return;
}

void TimerPrint()
{
    Timer* t;
    int    i;

    pthread_mutex_lock(&timerq.mutex);

    for (t = timerq.first, i = 0; t != NULL; t = t->next, i++) {
        printf("Timer %d: id=<%d>, expire=<%u,%u>, in_use=<%d>, cancelled=<%d>\n", i, t->timerId,
               (unsigned int)t->timeout.tv_sec, (unsigned int)t->timeout.tv_usec, t->in_use, t->cancelled);
    }

    pthread_mutex_unlock(&timerq.mutex);
    return;
}
