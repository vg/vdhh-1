/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu-common.h"
#include "qemu/timer.h"
#include "qemu/sockets.h"	// struct in_addr needed for libslirp.h
#include "slirp/libslirp.h"
#include "qemu/main-loop.h"
#include "aio.h"

#ifndef _WIN32

struct vmx_signalfd_siginfo {
    uint32_t ssi_signo;   /* Signal number */
    int32_t  ssi_errno;   /* Error number (unused) */
    int32_t  ssi_code;    /* Signal code */
    uint32_t ssi_pid;     /* PID of sender */
    uint32_t ssi_uid;     /* Real UID of sender */
    int32_t  ssi_fd;      /* File descriptor (SIGIO) */
    uint32_t ssi_tid;     /* Kernel timer ID (POSIX timers) */
    uint32_t ssi_band;    /* Band event (SIGIO) */
    uint32_t ssi_overrun; /* POSIX timer overrun count */
    uint32_t ssi_trapno;  /* Trap number that caused signal */
    int32_t  ssi_status;  /* Exit status or signal (SIGCHLD) */
    int32_t  ssi_int;     /* Integer sent by sigqueue(2) */
    uint64_t ssi_ptr;     /* Pointer sent by sigqueue(2) */
    uint64_t ssi_utime;   /* User GETCPU time consumed (SIGCHLD) */
    uint64_t ssi_stime;   /* System GETCPU time consumed (SIGCHLD) */
    uint64_t ssi_addr;    /* Address that generated signal
                           (for hardware-generated signals) */
    uint8_t  pad[48];     /* Pad size to 128 bytes (allow for
                           additional fields in the future) */
};

struct sigfd_compat_info
{
    sigset_t mask;
    int fd;
};

/* If we have signalfd, we mask out the signals we want to handle and then
 * use signalfd to listen for them.  We rely on whatever the current signal
 * handler is to dispatch the signals when we receive them.
 */
static void sigfd_handler(void *opaque)
{
    int fd = (intptr_t)opaque;
    struct vmx_signalfd_siginfo info;
    struct sigaction action;
    ssize_t len;

    while (1) {
        do {
            len = read(fd, &info, sizeof(info));
        } while (len == -1 && errno == EINTR);

        if (len == -1 && errno == EAGAIN) {
            break;
        }

        if (len != sizeof(info)) {
            printf("read from sigfd returned %zd: %m\n", len);
            return;
        }

        sigaction(info.ssi_signo, NULL, &action);
        if ((action.sa_flags & SA_SIGINFO) && action.sa_sigaction) {
            action.sa_sigaction(info.ssi_signo,
                                (siginfo_t *)&info, NULL);
        } else if (action.sa_handler) {
            action.sa_handler(info.ssi_signo);
        }
    }
}

void *vmx_sigwaitfd(struct sigfd_compat_info *info)
{
    while (1) {
        int sig;
        int err;
        
        err = sigwait(&info->mask, &sig);
        if (err != 0) {
            if (errno == EINTR) {
                continue;
            } else {
                return NULL;
            }
        } else {
            struct vmx_signalfd_siginfo buffer;
            size_t offset = 0;
            
            memset(&buffer, 0, sizeof(buffer));
            buffer.ssi_signo = sig;
            
            while (offset < sizeof(buffer)) {
                ssize_t len;
                
                len = write(info->fd, (char *)&buffer + offset,
                            sizeof(buffer) - offset);
                if (len == -1 && errno == EINTR)
                    continue;
                
                if (len <= 0) {
                    return NULL;
                }
                
                offset += len;
            }
        }
    }
}

int vmx_signalfd(const sigset_t *mask)
{
    struct sigfd_compat_info *info;
    QemuThread thread;
    int fds[2];
    
    info = malloc(sizeof(*info));
    if (info == NULL) {
        errno = ENOMEM;
        return -1;
    }
    
    if (pipe(fds) == -1) {
        free(info);
        return -1;
    }
    
    vmx_set_cloexec(fds[0]);
    vmx_set_cloexec(fds[1]);
    
    memcpy(&info->mask, mask, sizeof(*mask));
    info->fd = fds[1];
    
    vmx_thread_create(&thread, "signalfd_compat", vmx_sigwaitfd, info,
                       QEMU_THREAD_DETACHED);
    
    return fds[0];
}

static int vmx_signal_init(void)
{
    int sigfd;
    sigset_t set;

    /*
     * SIG_IPI must be blocked in the main thread and must not be caught
     * by sigwait() in the signal thread. Otherwise, the cpu thread will
     * not catch it reliably.
     */
    sigemptyset(&set);
    sigaddset(&set, SIG_IPI);
    sigaddset(&set, SIGIO);
    sigaddset(&set, SIGALRM);
    sigaddset(&set, SIGBUS);
    /* SIGINT cannot be handled via signalfd, so that ^C can be used
     * to interrupt QEMU when it is being run under gdb.  SIGHUP and
     * SIGTERM are also handled asynchronously, even though it is not
     * strictly necessary, because they use the same handler as SIGINT.
     */
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    sigdelset(&set, SIG_IPI);
    sigfd = vmx_signalfd(&set);
    if (sigfd == -1) {
        fprintf(stderr, "failed to create signalfd\n");
        return -errno;
    }

    fcntl_setfl(sigfd, O_NONBLOCK);

    vmx_set_fd_handler2(sigfd, NULL, sigfd_handler, NULL,
                         (void *)(intptr_t)sigfd);

    return 0;
}

#else /* _WIN32 */

static int vmx_signal_init(void)
{
    return 0;
}
#endif

static VeertuAioContext *vmx_aio_context;

VeertuAioContext *vmx_get_aio_context(void)
{
    return vmx_aio_context;
}

void vmx_notify_event(void)
{
    if (!vmx_aio_context) {
        return;
    }
    aio_notify(vmx_aio_context);
}

static GArray *gpollfds;

int vmx_process_events_init(Error **errp)
{
    int ret;
    GSource *src;
    Error *local_error = NULL;

    init_clocks();

    ret = vmx_signal_init();
    if (ret) {
        return ret;
    }

    vmx_aio_context = aio_context_new(&local_error);
    if (!vmx_aio_context) {
        error_propagate(errp, local_error);
        return -EMFILE;
    }
    gpollfds = g_array_new(FALSE, FALSE, sizeof(GPollFD));
    src = aio_get_g_source(vmx_aio_context);
    g_source_attach(src, NULL);
    g_source_unref(src);
    return 0;
}

static int max_priority;

#ifndef _WIN32
static int glib_pollfds_idx;
static int glib_n_poll_fds;

static void glib_pollfds_fill(int64_t *cur_timeout)
{
    GMainContext *context = g_main_context_default();
    int timeout = 0;
    int64_t timeout_ns;
    int n;

    g_main_context_prepare(context, &max_priority);

    glib_pollfds_idx = gpollfds->len;
    n = glib_n_poll_fds;
    do {
        GPollFD *pfds;
        glib_n_poll_fds = n;
        g_array_set_size(gpollfds, glib_pollfds_idx + glib_n_poll_fds);
        pfds = &g_array_index(gpollfds, GPollFD, glib_pollfds_idx);
        n = g_main_context_query(context, max_priority, &timeout, pfds,
                                 glib_n_poll_fds);
    } while (n != glib_n_poll_fds);

    if (timeout < 0) {
        timeout_ns = -1;
    } else {
        timeout_ns = (int64_t)timeout * (int64_t)SCALE_MS;
    }

    *cur_timeout = vmx_soonest_timeout(timeout_ns, *cur_timeout);
}

static void glib_pollfds_poll(void)
{
    GMainContext *context = g_main_context_default();
    GPollFD *pfds = &g_array_index(gpollfds, GPollFD, glib_pollfds_idx);

    if (g_main_context_check(context, max_priority, pfds, glib_n_poll_fds)) {
        g_main_context_dispatch(context);
    }
}

#define MAX_MAIN_LOOP_SPIN (1000)

static int vmx_event_wait_ns(int64_t timeout)
{
    int ret;
    static int spin_counter;

    glib_pollfds_fill(&timeout);

    /* If the I/O thread is very busy or we are incorrectly busy waiting in
     * the I/O thread, this can lead to starvation of the BQL such that the
     * VCPU threads never run.  To make sure we can detect the later case,
     * print a message to the screen.  If we run into this condition, create
     * a fake timeout in order to give the VCPU threads a chance to run.
     */
    if (!timeout && (spin_counter > MAX_MAIN_LOOP_SPIN)) {
        static bool notified;

        if (!notified) {
            fprintf(stderr,
                    "main-loop: WARNING: I/O thread spun for %d iterations\n",
                    MAX_MAIN_LOOP_SPIN);
            notified = true;
        }

        timeout = SCALE_MS;
    }

    if (timeout) {
        spin_counter = 0;
        vmx_mutex_unlock_iothread();
    } else {
        spin_counter++;
    }

    ret = vmx_poll_ns((GPollFD *)gpollfds->data, gpollfds->len, timeout);

    if (timeout) {
        vmx_mutex_lock_iothread();
    }

    glib_pollfds_poll();
    return ret;
}
#else
/***********************************************************/
/* Polling handling */

typedef struct PollingEntry {
    PollingFunc *func;
    void *opaque;
    struct PollingEntry *next;
} PollingEntry;

static PollingEntry *first_polling_entry;

int vmx_add_polling_cb(PollingFunc *func, void *opaque)
{
    PollingEntry **ppe, *pe;
    pe = g_malloc0(sizeof(PollingEntry));
    pe->func = func;
    pe->opaque = opaque;
    for(ppe = &first_polling_entry; *ppe != NULL; ppe = &(*ppe)->next);
    *ppe = pe;
    return 0;
}

void vmx_del_polling_cb(PollingFunc *func, void *opaque)
{
    PollingEntry **ppe, *pe;
    for(ppe = &first_polling_entry; *ppe != NULL; ppe = &(*ppe)->next) {
        pe = *ppe;
        if (pe->func == func && pe->opaque == opaque) {
            *ppe = pe->next;
            g_free(pe);
            break;
        }
    }
}

/***********************************************************/
/* Wait objects support */
typedef struct WaitObjects {
    int num;
    int revents[MAXIMUM_WAIT_OBJECTS + 1];
    HANDLE events[MAXIMUM_WAIT_OBJECTS + 1];
    WaitObjectFunc *func[MAXIMUM_WAIT_OBJECTS + 1];
    void *opaque[MAXIMUM_WAIT_OBJECTS + 1];
} WaitObjects;

static WaitObjects wait_objects = {0};

int vmx_add_wait_object(HANDLE handle, WaitObjectFunc *func, void *opaque)
{
    WaitObjects *w = &wait_objects;
    if (w->num >= MAXIMUM_WAIT_OBJECTS) {
        return -1;
    }
    w->events[w->num] = handle;
    w->func[w->num] = func;
    w->opaque[w->num] = opaque;
    w->revents[w->num] = 0;
    w->num++;
    return 0;
}

void vmx_del_wait_object(HANDLE handle, WaitObjectFunc *func, void *opaque)
{
    int i, found;
    WaitObjects *w = &wait_objects;

    found = 0;
    for (i = 0; i < w->num; i++) {
        if (w->events[i] == handle) {
            found = 1;
        }
        if (found) {
            w->events[i] = w->events[i + 1];
            w->func[i] = w->func[i + 1];
            w->opaque[i] = w->opaque[i + 1];
            w->revents[i] = w->revents[i + 1];
        }
    }
    if (found) {
        w->num--;
    }
}

void vmx_fd_register(int fd)
{
    WSAEventSelect(fd, veertu_event_notifier_get_handle(&vmx_aio_context->notifier),
                   FD_READ | FD_ACCEPT | FD_CLOSE |
                   FD_CONNECT | FD_WRITE | FD_OOB);
}

static int pollfds_fill(GArray *pollfds, fd_set *rfds, fd_set *wfds,
                        fd_set *xfds)
{
    int nfds = -1;
    int i;

    for (i = 0; i < pollfds->len; i++) {
        GPollFD *pfd = &g_array_index(pollfds, GPollFD, i);
        int fd = pfd->fd;
        int events = pfd->events;
        if (events & G_IO_IN) {
            FD_SET(fd, rfds);
            nfds = MAX(nfds, fd);
        }
        if (events & G_IO_OUT) {
            FD_SET(fd, wfds);
            nfds = MAX(nfds, fd);
        }
        if (events & G_IO_PRI) {
            FD_SET(fd, xfds);
            nfds = MAX(nfds, fd);
        }
    }
    return nfds;
}

static void pollfds_poll(GArray *pollfds, int nfds, fd_set *rfds,
                         fd_set *wfds, fd_set *xfds)
{
    int i;

    for (i = 0; i < pollfds->len; i++) {
        GPollFD *pfd = &g_array_index(pollfds, GPollFD, i);
        int fd = pfd->fd;
        int revents = 0;

        if (FD_ISSET(fd, rfds)) {
            revents |= G_IO_IN;
        }
        if (FD_ISSET(fd, wfds)) {
            revents |= G_IO_OUT;
        }
        if (FD_ISSET(fd, xfds)) {
            revents |= G_IO_PRI;
        }
        pfd->revents = revents & pfd->events;
    }
}

static int vmx_event_wait(int64_t timeout)
{
    GMainContext *context = g_main_context_default();
    GPollFD poll_fds[1024 * 2]; /* this is probably overkill */
    int select_ret = 0;
    int g_poll_ret, ret, i, n_poll_fds;
    PollingEntry *pe;
    WaitObjects *w = &wait_objects;
    gint poll_timeout;
    int64_t poll_timeout_ns;
    static struct timeval tv0;
    fd_set rfds, wfds, xfds;
    int nfds;

    /* XXX: need to suppress polling by better using win32 events */
    ret = 0;
    for (pe = first_polling_entry; pe != NULL; pe = pe->next) {
        ret |= pe->func(pe->opaque);
    }
    if (ret != 0) {
        return ret;
    }

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&xfds);
    nfds = pollfds_fill(gpollfds, &rfds, &wfds, &xfds);
    if (nfds >= 0) {
        select_ret = select(nfds + 1, &rfds, &wfds, &xfds, &tv0);
        if (select_ret != 0) {
            timeout = 0;
        }
        if (select_ret > 0) {
            pollfds_poll(gpollfds, nfds, &rfds, &wfds, &xfds);
        }
    }

    g_main_context_prepare(context, &max_priority);
    n_poll_fds = g_main_context_query(context, max_priority, &poll_timeout,
                                      poll_fds, ARRAY_SIZE(poll_fds));
    g_assert(n_poll_fds <= ARRAY_SIZE(poll_fds));

    for (i = 0; i < w->num; i++) {
        poll_fds[n_poll_fds + i].fd = (DWORD_PTR)w->events[i];
        poll_fds[n_poll_fds + i].events = G_IO_IN;
    }

    if (poll_timeout < 0) {
        poll_timeout_ns = -1;
    } else {
        poll_timeout_ns = (int64_t)poll_timeout * (int64_t)SCALE_MS;
    }

    poll_timeout_ns = vmx_soonest_timeout(poll_timeout_ns, timeout);

    vmx_mutex_unlock_iothread();
    g_poll_ret = vmx_poll_ns(poll_fds, n_poll_fds + w->num, poll_timeout_ns);

    vmx_mutex_lock_iothread();
    if (g_poll_ret > 0) {
        for (i = 0; i < w->num; i++) {
            w->revents[i] = poll_fds[n_poll_fds + i].revents;
        }
        for (i = 0; i < w->num; i++) {
            if (w->revents[i] && w->func[i]) {
                w->func[i](w->opaque[i]);
            }
        }
    }

    if (g_main_context_check(context, max_priority, poll_fds, n_poll_fds)) {
        g_main_context_dispatch(context);
    }

    return select_ret || g_poll_ret;
}
#endif

int main_loop_wait(int nonblocking)
{
    int ret;
    uint32_t timeout = UINT32_MAX;
    int64_t timeout_ns;

    if (nonblocking) {
        timeout = 0;
    }

    /* poll any events */
    g_array_set_size(gpollfds, 0); /* reset for new iteration */
    /* XXX: separate device handlers from system ones */
#ifdef CONFIG_SLIRP
    slirp_pollfds_fill(gpollfds, &timeout);
#endif
    vmx_iohandler_fill(gpollfds);

    if (timeout == UINT32_MAX) {
        timeout_ns = -1;
    } else {
        timeout_ns = (uint64_t)timeout * (int64_t)(SCALE_MS);
    }

    timeout_ns = vmx_soonest_timeout(timeout_ns,
                                      timerlistgroup_deadline_ns(
                                          &main_loop_tlg));

    ret = vmx_event_wait_ns(timeout_ns);
    vmx_iohandler_poll(gpollfds, ret);
#ifdef CONFIG_SLIRP
    slirp_pollfds_poll(gpollfds, (ret < 0));
#endif

    vmx_clock_run_all_timers();

    return ret;
}

/* Functions to operate on the main QEMU VeertuAioContext.  */

QEMUBH *vmx_bh_new(QEMUBHFunc *cb, void *opaque)
{
    return aio_bh_new(vmx_aio_context, cb, opaque);
}