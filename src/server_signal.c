#include "server_signal.h"
#include "server_state.h"
#include <p101_c/p101_string.h>
#include <p101_process/p101_sched.h>
#include <p101_process/p101_setjmp.h>
#include <p101_process/p101_signal.h>
#include <p101_process/p101_spawn.h>
#include <p101_process/p101_stdio.h>
#include <p101_process/p101_stdlib.h>
#include <p101_process/p101_unistd.h>
#include <p101_process/sys/p101_resource.h>
#include <p101_process/sys/p101_times.h>
#include <p101_process/sys/p101_wait.h>
#include <signal.h>

static void sigint_handler(int signum);

void setup_signal_handler(const struct p101_env *env, struct p101_error *err)
{
    struct sigaction sa;
    bool             has_error;

    P101_TRACE_SCOPE(env);
    p101_memset(env, &sa, 0, sizeof(sa));

#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#endif
    sa.sa_handler = sigint_handler;
#ifdef __clang__
    #pragma clang diagnostic pop
#endif

    p101_sigemptyset(env, err, &sa.sa_mask);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto done;
    }

    sa.sa_flags = 0;
    p101_sigaction(env, err, SIGINT, &sa, NULL);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto done;
    }

    p101_memset(env, &sa, 0, sizeof(sa));

#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#endif
    sa.sa_handler = SIG_IGN;
#ifdef __clang__
    #pragma clang diagnostic pop
#endif

    p101_sigemptyset(env, err, &sa.sa_mask);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto done;
    }

    sa.sa_flags = 0;
    p101_sigaction(env, err, SIGPIPE, &sa, NULL);

done:
    return;
}

static void sigint_handler(const int signum)
{
    (void)signum;
    server_request_exit();
}
