#ifndef P2P_PLATFORM_H
#define P2P_PLATFORM_H

/*
 * Cross-platform compatibility layer.
 *
 * Provides unified APIs for threads, networking, time, and command-line
 * parsing across Linux and Windows.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
/* ================================================================
 *  Windows
 * ================================================================ */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

/* ---------- threads ---------- */

typedef HANDLE              p2p_thread_t;
typedef CRITICAL_SECTION    p2p_mutex_t;

typedef struct {
    CONDITION_VARIABLE cv;
} p2p_cond_t;

typedef DWORD (WINAPI *p2p_thread_func_t)(LPVOID);

static inline int p2p_mutex_init(p2p_mutex_t *m)
{ InitializeCriticalSection(m); return 0; }

static inline int p2p_mutex_destroy(p2p_mutex_t *m)
{ DeleteCriticalSection(m); return 0; }

static inline int p2p_mutex_lock(p2p_mutex_t *m)
{ EnterCriticalSection(m); return 0; }

static inline int p2p_mutex_unlock(p2p_mutex_t *m)
{ LeaveCriticalSection(m); return 0; }

static inline int p2p_cond_init(p2p_cond_t *c)
{ InitializeConditionVariable(&c->cv); return 0; }

static inline int p2p_cond_destroy(p2p_cond_t *c)
{ (void)c; return 0; }

static inline int p2p_cond_signal(p2p_cond_t *c)
{ WakeConditionVariable(&c->cv); return 0; }

static inline int p2p_cond_broadcast(p2p_cond_t *c)
{ WakeAllConditionVariable(&c->cv); return 0; }

static inline int p2p_cond_wait(p2p_cond_t *c, p2p_mutex_t *m)
{ SleepConditionVariableCS(&c->cv, m, INFINITE); return 0; }

static inline int p2p_cond_timedwait_us(p2p_cond_t *c, p2p_mutex_t *m, uint64_t timeout_us)
{
    DWORD ms = (DWORD)(timeout_us / 1000);
    if (ms == 0) ms = 1;
    SleepConditionVariableCS(&c->cv, m, ms);
    return 0;
}

static inline int p2p_thread_create(p2p_thread_t *t, void *(*func)(void *), void *arg)
{
    *t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)func, arg, 0, NULL);
    return (*t != NULL) ? 0 : -1;
}

static inline int p2p_thread_join(p2p_thread_t t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

/* ---------- networking ---------- */

typedef SOCKET p2p_socket_t;
#define P2P_INVALID_SOCKET  INVALID_SOCKET
#define P2P_SOCKET_ERROR    SOCKET_ERROR

static inline int p2p_net_init(void)
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
}

static inline void p2p_net_cleanup(void)
{ WSACleanup(); }

static inline int p2p_socket_close(p2p_socket_t s)
{ return closesocket(s); }

static inline int p2p_socket_shutdown(p2p_socket_t s)
{ return shutdown(s, SD_BOTH); }

static inline int p2p_socket_send(p2p_socket_t s, const void *buf, int len)
{ return send(s, (const char *)buf, len, 0); }

static inline int p2p_socket_recv(p2p_socket_t s, void *buf, int len)
{ return recv(s, (char *)buf, len, 0); }

/* ---------- time ---------- */

static inline uint64_t p2p_monotonic_us(void)
{
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart * 1000000ULL / freq.QuadPart);
}

static inline uint64_t p2p_realtime_us(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    /* FILETIME is in 100ns intervals since 1601-01-01. Convert to us. */
    return uli.QuadPart / 10;
}

/* ---------- sleep ---------- */

static inline void p2p_sleep_ms(unsigned int ms) { Sleep(ms); }
static inline void p2p_usleep(unsigned int us)   { Sleep(us / 1000 + (us % 1000 ? 1 : 0)); }

/* ---------- atomic ---------- */

#define P2P_ATOMIC_INT long

static inline uint32_t p2p_atomic_fetch_add(volatile long *ptr, long val)
{ return (uint32_t)InterlockedExchangeAdd(ptr, val); }

/* ---------- signal ---------- */

/* Applications should call p2p_set_ctrl_handler() with their own flag pointer. */
typedef volatile int *p2p_quit_flag_ptr;
static p2p_quit_flag_ptr _p2p_quit_flag = NULL;

static inline BOOL WINAPI _p2p_console_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (_p2p_quit_flag) *_p2p_quit_flag = 0;
        return TRUE;
    }
    return FALSE;
}

static inline void p2p_set_ctrl_handler(volatile int *running_flag)
{
    _p2p_quit_flag = running_flag;
    SetConsoleCtrlHandler(_p2p_console_handler, TRUE);
}

/* ---------- getopt ---------- */

/*
 * Minimal getopt_long for Windows.  Supports long options with '=' or
 * space-separated arguments and short options.
 */

#ifndef no_argument
#define no_argument        0
#endif
#ifndef required_argument
#define required_argument  1
#endif
#ifndef optional_argument
#define optional_argument  2
#endif

struct p2p_option {
    const char *name;
    int         has_arg;
    int        *flag;
    int         val;
};

static char *p2p_optarg = NULL;
static int   p2p_optind = 1;

static inline int p2p_getopt_long(int argc, char *const argv[],
                                  const char *shortopts,
                                  const struct p2p_option *longopts,
                                  int *longindex)
{
    if (p2p_optind >= argc) return -1;
    char *arg = argv[p2p_optind];

    /* Long option: --name or --name=value */
    if (arg[0] == '-' && arg[1] == '-' && arg[2] != '\0') {
        const char *name = arg + 2;
        const char *eq = strchr(name, '=');
        size_t nlen = eq ? (size_t)(eq - name) : strlen(name);

        for (int i = 0; longopts[i].name; i++) {
            if (strlen(longopts[i].name) == nlen &&
                strncmp(longopts[i].name, name, nlen) == 0) {
                p2p_optind++;
                if (longopts[i].has_arg) {
                    if (eq) {
                        p2p_optarg = (char *)(eq + 1);
                    } else if (p2p_optind < argc) {
                        p2p_optarg = argv[p2p_optind++];
                    } else {
                        return '?';
                    }
                }
                if (longindex) *longindex = i;
                return longopts[i].val;
            }
        }
        p2p_optind++;
        return '?';
    }

    /* Short option: -X or -Xvalue */
    if (arg[0] == '-' && arg[1] != '-' && arg[1] != '\0') {
        char ch = arg[1];
        const char *p = strchr(shortopts, ch);
        if (!p) { p2p_optind++; return '?'; }
        p2p_optind++;
        if (*(p + 1) == ':') {
            if (arg[2] != '\0') {
                p2p_optarg = arg + 2;
            } else if (p2p_optind < argc) {
                p2p_optarg = argv[p2p_optind++];
            } else {
                return '?';
            }
        }
        return ch;
    }

    return -1;
}

#else
/* ================================================================
 *  POSIX / Linux
 * ================================================================ */

#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ---------- threads ---------- */

typedef pthread_t       p2p_thread_t;
typedef pthread_mutex_t p2p_mutex_t;
typedef pthread_cond_t  p2p_cond_t;

#define p2p_mutex_init(m)       pthread_mutex_init(m, NULL)
#define p2p_mutex_destroy(m)    pthread_mutex_destroy(m)
#define p2p_mutex_lock(m)       pthread_mutex_lock(m)
#define p2p_mutex_unlock(m)     pthread_mutex_unlock(m)

#define p2p_cond_init(c)        pthread_cond_init(c, NULL)
#define p2p_cond_destroy(c)     pthread_cond_destroy(c)
#define p2p_cond_signal(c)      pthread_cond_signal(c)
#define p2p_cond_broadcast(c)   pthread_cond_broadcast(c)
#define p2p_cond_wait(c, m)     pthread_cond_wait(c, m)

static inline int p2p_cond_timedwait_us(p2p_cond_t *c, p2p_mutex_t *m, uint64_t timeout_us)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)(timeout_us * 1000);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec  += ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
    }
    return pthread_cond_timedwait(c, m, &ts);
}

static inline int p2p_thread_create(p2p_thread_t *t, void *(*func)(void *), void *arg)
{ return pthread_create(t, NULL, func, arg); }

static inline int p2p_thread_join(p2p_thread_t t)
{ return pthread_join(t, NULL); }

/* ---------- networking ---------- */

typedef int p2p_socket_t;
#define P2P_INVALID_SOCKET  (-1)
#define P2P_SOCKET_ERROR    (-1)

static inline int  p2p_net_init(void) { return 0; }
static inline void p2p_net_cleanup(void) {}

static inline int p2p_socket_close(p2p_socket_t s) { return close(s); }
static inline int p2p_socket_shutdown(p2p_socket_t s) { return shutdown(s, SHUT_RDWR); }

static inline int p2p_socket_send(p2p_socket_t s, const void *buf, int len)
{ return (int)write(s, buf, len); }

static inline int p2p_socket_recv(p2p_socket_t s, void *buf, int len)
{ return (int)read(s, buf, len); }

/* ---------- time ---------- */

static inline uint64_t p2p_monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static inline uint64_t p2p_realtime_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ---------- sleep ---------- */

static inline void p2p_sleep_ms(unsigned int ms) { usleep(ms * 1000u); }
static inline void p2p_usleep(unsigned int us)   { usleep(us); }

/* ---------- atomic ---------- */

#define P2P_ATOMIC_INT int

static inline uint32_t p2p_atomic_fetch_add(volatile int *ptr, int val)
{ return (uint32_t)__sync_fetch_and_add(ptr, val); }

/* ---------- signal ---------- */

static inline void p2p_set_ctrl_handler(volatile int *running_flag)
{
    (void)running_flag;
    /* On Linux, callers use signal() / sigaction() directly. */
}

/* ---------- getopt ---------- */

#include <getopt.h>

/* Map plan names to system getopt names so call sites are identical. */
#define p2p_option      option
#define p2p_optarg      optarg
#define p2p_optind      optind
#define p2p_getopt_long getopt_long

#endif /* _WIN32 / POSIX */

#endif /* P2P_PLATFORM_H */
