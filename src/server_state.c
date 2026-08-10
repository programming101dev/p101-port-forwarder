#include "server_state.h"

static volatile sig_atomic_t exit_flag      = 0;                            // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static pthread_mutex_t       lock           = PTHREAD_MUTEX_INITIALIZER;    // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static pthread_cond_t        cond           = PTHREAD_COND_INITIALIZER;     // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
static atomic_uint           active_threads = 0;                            // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

bool server_exit_requested(void)
{
    return exit_flag != 0;
}

void server_request_exit(void)
{
    exit_flag = 1;
}

pthread_mutex_t *server_lock(void)
{
    return &lock;
}

pthread_cond_t *server_cond(void)
{
    return &cond;
}

void server_active_threads_reset(const struct p101_env *env)
{
    p101_atomic_uint_store(env, &active_threads, 0);
}

unsigned int server_active_threads_load(const struct p101_env *env)
{
    unsigned int value;

    value = p101_atomic_uint_load(env, &active_threads);
    return value;
}

void server_active_threads_increment(const struct p101_env *env)
{
    p101_atomic_uint_fetch_add(env, &active_threads, 1);
}

void server_active_threads_decrement(const struct p101_env *env)
{
    p101_atomic_uint_fetch_sub(env, &active_threads, 1);
}
