#include "server_connection.h"
#include "server_state.h"
#include "settings.h"
#include <errno.h>
#include <p101_c/p101_stdatomic.h>
#include <p101_c/p101_stdio.h>
#include <p101_filesystem/p101_dirent.h>
#include <p101_filesystem/p101_fnmatch.h>
#include <p101_filesystem/p101_ftw.h>
#include <p101_filesystem/p101_glob.h>
#include <p101_filesystem/p101_libgen.h>
#include <p101_filesystem/p101_stdio.h>
#include <p101_filesystem/p101_stdlib.h>
#include <p101_filesystem/p101_unistd.h>
#include <p101_filesystem/sys/p101_stat.h>
#include <p101_filesystem/sys/p101_statvfs.h>
#include <p101_io/p101_aio.h>
#include <p101_io/p101_fcntl.h>
#include <p101_io/p101_poll.h>
#include <p101_io/p101_stdio.h>
#include <p101_io/p101_unistd.h>
#include <p101_io/sys/p101_select.h>
#include <p101_io/sys/p101_uio.h>
#include <p101_network/arpa/p101_inet.h>
#include <p101_network/net/p101_ethernet.h>
#include <p101_network/net/p101_if.h>
#include <p101_network/p101_ifaddrs.h>
#include <p101_network/p101_netdb.h>
#include <p101_network/sys/p101_socket.h>
#include <p101_random/p101_stdlib.h>
#include <p101_sync/p101_pthread.h>
#include <p101_sync/p101_semaphore.h>
#include <p101_thread/p101_pthread.h>
#include <p101_thread/p101_signal.h>
#include <p101_time/p101_time.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>

#ifndef BUFFER_LEN
    #define BUFFER_LEN ((size_t)10240 * (size_t)10)
#endif

static bool  start_copy_thread(const struct p101_env *env, struct p101_error *err, pthread_t *forwarder_thread, struct copy_data *data, const struct settings *sets, int from_socket, int to_socket);
static void *copy_handler(void *arg);
static bool  copy(const struct p101_env *env, struct p101_error *err, int to_fd, int from_fd, const struct settings *sets);
static bool  error_is_connection_closed(const struct p101_error *err);
static bool  error_is_connection_local(const struct p101_error *err);
static bool  error_is_retryable(const struct p101_error *err);
static void  shutdown_socket(const struct p101_env *env, struct p101_error *err, int socket, int how);
static void  close_socket(const struct p101_env *env, struct p101_error *err, int *socket);
static void  delay(const struct p101_env *env, struct p101_error *err, time_t min_seconds, time_t max_seconds, long min_nanoseconds, long max_nanoseconds);
static long  generate_random_long(const struct p101_env *env, long min, long max);

void handle_connection(const struct p101_env *env, struct p101_error *err, void *arg, struct p101_fsm_effect_sink *sink, struct p101_fsm_decision *decision)
{
    struct server_data *data;
    p101_fsm_state_id   next_state;
    socklen_t           addr_len;
    in_port_t           net_port;
    pthread_t           from_forwarder = p101_pthread_self(env);
    struct copy_data    from_data;
    pthread_t           to_forwarder = p101_pthread_self(env);
    struct copy_data    to_data;
    bool                from_started;
    bool                to_started;
    struct p101_error  *cleanup_err;
    struct p101_error  *operation_err;
    struct p101_error  *optional_error;
    pthread_mutex_t    *mutex;
    pthread_cond_t     *condition;
    unsigned int        active_threads;
    bool                has_error;
    bool                exit_requested;
    bool                local_error;
    const char         *message;
    int                 forward_socket;
    int                 unlock_result;

    P101_TRACE_SCOPE(env);
    (void)sink;
    p101_printf(env, err, "Handing connection\n");
    data         = (struct server_data *)arg;
    from_started = false;
    to_started   = false;
    cleanup_err  = NULL;

    forward_socket       = p101_socket(env, err, data->sets->addr_out.ss_family, SOCK_STREAM, 0);
    data->forward_socket = forward_socket;

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto error;
    }

    net_port = p101_htons(env, data->sets->port_out);

    if(data->sets->addr_out.ss_family == AF_INET)
    {
        struct sockaddr_in *ipv4_addr;

        ipv4_addr           = (struct sockaddr_in *)&data->sets->addr_out;
        addr_len            = sizeof(*ipv4_addr);
        ipv4_addr->sin_port = net_port;
    }
    else if(data->sets->addr_out.ss_family == AF_INET6)
    {
        struct sockaddr_in6 *ipv6_addr;

        ipv6_addr            = (struct sockaddr_in6 *)&data->sets->addr_out;
        addr_len             = sizeof(*ipv6_addr);
        ipv6_addr->sin6_port = net_port;
    }
    else
    {
        P101_ERROR_RAISE_USER(err, "Internal error: addr->ss_family must be AF_INET or AF_INET6", 1);
        goto error;
    }

    p101_printf(env, err, "Connecting to server\n");
    p101_connect(env, err, data->forward_socket, (struct sockaddr *)&data->sets->addr_out, addr_len);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto error;
    }

    p101_printf(env, err, "Connected to server\n");
    server_active_threads_reset(env);
    from_started = start_copy_thread(env, err, &from_forwarder, &from_data, data->sets, data->forward_socket, data->client_socket);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto error;
    }

    to_started = start_copy_thread(env, err, &to_forwarder, &to_data, data->sets, data->client_socket, data->forward_socket);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto error;
    }

    // wait for a thread to signal the condition
    mutex = server_lock();
    p101_pthread_mutex_lock(env, err, mutex);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto error;
    }

    //    p101_printf(env, err, "active threads = %u\n", server_active_threads_load(env));

    active_threads = server_active_threads_load(env);
    while(active_threads > 0U)
    {
        p101_printf(env, err, "active threads = %u\n", active_threads);
        p101_printf(env, err, "waiting on condition\n");
        condition = server_cond();
        mutex     = server_lock();
        p101_pthread_cond_wait(env, err, condition, mutex);
        has_error = p101_error_has_error(err);
        if(has_error)
        {
            mutex         = server_lock();
            unlock_result = p101_pthread_mutex_unlock(env, err, mutex);
            (void)unlock_result;
            goto error;
        }
        p101_printf(env, err, "condition done\n");
        active_threads = server_active_threads_load(env);
    }

    mutex = server_lock();
    p101_pthread_mutex_unlock(env, err, mutex);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto error;
    }

    // wait for a thread to finish
    if(from_started)
    {
        p101_pthread_join(env, err, from_forwarder, NULL);
    }

    if(to_started)
    {
        p101_pthread_join(env, err, to_forwarder, NULL);
    }

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto error;
    }

    close_socket(env, err, &data->client_socket);
    close_socket(env, err, &data->forward_socket);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto error;
    }

    exit_requested = server_exit_requested();
    if(exit_requested)
    {
        next_state = CLEANUP;
    }
    else
    {
        next_state = ACCEPT;
    }
    goto done;

error:
    cleanup_err   = p101_error_create(false);
    operation_err = cleanup_err == NULL ? err : cleanup_err;
    shutdown_socket(env, operation_err, data->client_socket, SHUT_RDWR);
    shutdown_socket(env, operation_err, data->forward_socket, SHUT_RDWR);

    if(from_started)
    {
        p101_pthread_join(env, operation_err, from_forwarder, NULL);
    }

    if(to_started)
    {
        p101_pthread_join(env, operation_err, to_forwarder, NULL);
    }

    close_socket(env, operation_err, &data->client_socket);
    close_socket(env, operation_err, &data->forward_socket);
    if(cleanup_err != NULL)
    {
        has_error = p101_error_has_error(cleanup_err);
        if(has_error)
        {
            message = p101_error_get_message(cleanup_err);
            p101_fprintf(env, cleanup_err, stderr, "Connection cleanup error: %s\n", message);
        }
        p101_error_destroy(cleanup_err);
    }
    local_error    = error_is_connection_local(err);
    exit_requested = server_exit_requested();
    if(local_error && !exit_requested)
    {
        message        = p101_error_get_message(err);
        optional_error = P101_ERROR_OPTIONAL;
        p101_fprintf(env, optional_error, stderr, "Connection error: %s\n", message);
        p101_error_reset(err);
        next_state = ACCEPT;
    }
    else
    {
        next_state = CLEANUP;
    }

done:
    p101_printf(env, err, "Connection handled\n");
    p101_fsm_decide_transition(decision, next_state);
}

static bool start_copy_thread(const struct p101_env *env, struct p101_error *err, pthread_t *forwarder_thread, struct copy_data *data, const struct settings *sets, int from_socket, int to_socket)
{
    bool started;
    bool has_error;

    started       = false;
    data->env     = env;
    data->sets    = sets;
    data->from_fd = from_socket;
    data->to_fd   = to_socket;

    server_active_threads_increment(env);
    p101_pthread_create(env, err, forwarder_thread, NULL, copy_handler, data);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        server_active_threads_decrement(env);
    }
    else
    {
        started = true;
    }

    return started;
}

static void *copy_handler(void *arg)
{
    const struct p101_env *env;
    struct copy_data      *data;
    struct p101_error     *err;
    struct p101_error     *optional_error;
    bool                   closed;
    bool                   has_error;
    pthread_mutex_t       *mutex;
    pthread_cond_t        *condition;
    const char            *message;

    data = (struct copy_data *)arg;
    env  = data->env;
    err  = p101_error_create(false);

    if(err == NULL)
    {
        /* P101_ERROR_OPTIONAL rationale: the error object allocation itself failed. */
        optional_error = P101_ERROR_OPTIONAL;
        p101_fprintf(env, optional_error, stderr, "Unable to create copy thread error object\n");
        goto done;
    }

    do
    {
        const char *closed_str;

        closed = copy(env, err, data->to_fd, data->from_fd, data->sets);

        has_error = p101_error_has_error(err);
        if(has_error)
        {
            message = p101_error_get_message(err);
            p101_fprintf(env, err, stderr, "Copy thread error: %s\n", message);
            goto done;
        }

        if(closed)
        {
            closed_str = "true";
        }
        else
        {
            closed_str = "false";
        }

        p101_printf(env, err, "closed %d -> %d?: %s\n", data->from_fd, data->to_fd, closed_str);
    } while(!closed);

done:
    shutdown_socket(env, err, data->to_fd, SHUT_WR);
    p101_printf(env, err, "Ending thread\n");
    server_active_threads_decrement(env);
    mutex = server_lock();
    p101_pthread_mutex_lock(env, err, mutex);
    condition = server_cond();
    p101_pthread_cond_signal(env, err, condition);
    mutex = server_lock();
    p101_pthread_mutex_unlock(env, err, mutex);
    p101_error_destroy(err);

    return NULL;
}

static bool copy(const struct p101_env *env, struct p101_error *err, int to_fd, int from_fd, const struct settings *sets)
{
    uint8_t buffer[BUFFER_LEN];
    ssize_t bytes_read;
    bool    closed;
    bool    has_error;
    bool    retryable;
    bool    connection_closed;
    bool    exit_requested;

    closed = false;
    p101_printf(env, err, "Reading from %d to send to %d\n", from_fd, to_fd);
    bytes_read = p101_read(env, err, from_fd, buffer, BUFFER_LEN);
    p101_printf(env, err, "Read %zd from %d to send to %d\n", bytes_read, from_fd, to_fd);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        retryable = error_is_retryable(err);
        if(retryable)
        {
            p101_error_reset(err);
            exit_requested = server_exit_requested();
            if(exit_requested)
            {
                closed = true;
            }
        }
        else
        {
            connection_closed = error_is_connection_closed(err);
            if(connection_closed)
            {
                p101_error_reset(err);
                closed = true;
            }
        }

        goto done;
    }

    if(bytes_read == 0)
    {
        closed = true;
    }
    else
    {
        size_t bytes_remaining;
        size_t pos;

        bytes_remaining = (size_t)bytes_read;
        pos             = 0;

        do
        {
            size_t   bytes_to_write;
            ssize_t  bytes_written;
            uint32_t random_amount;

            if(sets->min_bytes == 0)
            {
                bytes_to_write = bytes_remaining;
            }
            else
            {
                if(sets->min_bytes == sets->max_bytes)
                {
                    bytes_to_write = sets->min_bytes;
                }
                else
                {
                    random_amount  = p101_arc4random_uniform(env, sets->max_bytes - sets->min_bytes + 1);
                    bytes_to_write = random_amount + sets->min_bytes;
                }

                if(bytes_to_write > bytes_remaining)
                {
                    bytes_to_write = bytes_remaining;
                }
            }

            // #pragma GCC diagnostic push
            // #pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
            bytes_written = p101_send(env, err, to_fd, &buffer[pos], bytes_to_write, 0);
            // #pragma GCC diagnostic pop

            has_error = p101_error_has_error(err);
            if(has_error)
            {
                retryable = error_is_retryable(err);
                if(retryable)
                {
                    p101_error_reset(err);
                    exit_requested = server_exit_requested();
                    if(exit_requested)
                    {
                        closed = true;
                        goto done;
                    }
                    continue;
                }

                connection_closed = error_is_connection_closed(err);
                if(connection_closed)
                {
                    p101_error_reset(err);
                    closed = true;
                }

                goto done;
            }

            if(bytes_written == 0)
            {
                closed = true;
                goto done;
            }

            if(sets->very_verbose)
            {
                p101_printf(env, err, "\n----\n");
                p101_fflush(env, err, stdout);
                p101_write(env, err, STDOUT_FILENO, &buffer[pos], (size_t)bytes_written);
                p101_fflush(env, err, stdout);
                p101_printf(env, err, "\n----\n");
            }
            has_error = p101_error_has_error(err);
            if(has_error)
            {
                goto done;
            }

            bytes_remaining -= (size_t)bytes_written;
            pos += (size_t)bytes_written;
            delay(env, err, sets->min_seconds, sets->max_seconds, sets->min_nanoseconds, sets->max_nanoseconds);

            has_error = p101_error_has_error(err);
            if(has_error)
            {
                retryable      = error_is_retryable(err);
                exit_requested = server_exit_requested();
                if(retryable && exit_requested)
                {
                    p101_error_reset(err);
                    closed = true;
                }

                goto done;
            }
        } while(bytes_remaining > 0);
    }

done:
    return closed;
}

#ifdef SIMPLE_PORT_FORWARDER_TESTING
bool server_copy_once_for_test(const struct p101_env *env, struct p101_error *err, int to_fd, int from_fd, const struct settings *sets)
{
    bool closed;

    closed = copy(env, err, to_fd, from_fd, sets);
    return closed;
}

bool server_connection_error_is_local_for_test(const struct p101_error *err)
{
    bool local;

    local = error_is_connection_local(err);
    return local;
}
#endif

static bool error_is_connection_closed(const struct p101_error *err)
{
    bool is_bad_descriptor;
    bool is_broken_pipe;
    bool is_reset;
    bool is_aborted;
    bool closed;

    is_bad_descriptor = p101_error_is_errno(err, EBADF);
    is_broken_pipe    = p101_error_is_errno(err, EPIPE);
    is_reset          = p101_error_is_errno(err, ECONNRESET);
    is_aborted        = p101_error_is_errno(err, ECONNABORTED);
    closed            = (is_bad_descriptor || is_broken_pipe || is_reset || is_aborted) != 0;
    return closed;
}

static bool error_is_connection_local(const struct p101_error *err)
{
    bool is_aborted;
    bool is_refused;
    bool is_reset;
    bool is_host_unreachable;
    bool is_network_down;
    bool is_network_unreachable;
    bool is_not_connected;
    bool is_broken_pipe;
    bool is_timed_out;
    bool local;

    is_aborted             = p101_error_is_errno(err, ECONNABORTED);
    is_refused             = p101_error_is_errno(err, ECONNREFUSED);
    is_reset               = p101_error_is_errno(err, ECONNRESET);
    is_host_unreachable    = p101_error_is_errno(err, EHOSTUNREACH);
    is_network_down        = p101_error_is_errno(err, ENETDOWN);
    is_network_unreachable = p101_error_is_errno(err, ENETUNREACH);
    is_not_connected       = p101_error_is_errno(err, ENOTCONN);
    is_broken_pipe         = p101_error_is_errno(err, EPIPE);
    is_timed_out           = p101_error_is_errno(err, ETIMEDOUT);
    local                  = (is_aborted || is_refused || is_reset || is_host_unreachable || is_network_down || is_network_unreachable || is_not_connected || is_broken_pipe || is_timed_out) != 0;
    return local;
}

static bool error_is_retryable(const struct p101_error *err)
{
    bool retryable;

    retryable = p101_error_is_errno(err, EINTR);
    return retryable;
}

static void shutdown_socket(const struct p101_env *env, struct p101_error *err, int socket, int how)
{
    if(socket != -1)
    {
        p101_shutdown(env, err, socket, how);
    }
}

static void close_socket(const struct p101_env *env, struct p101_error *err, int *socket)
{
    if(*socket != -1)
    {
        int socket_to_close;

        socket_to_close = *socket;
        *socket         = -1;
        p101_printf(env, err, "closing %d\n", socket_to_close);
        p101_close(env, err, socket_to_close);
    }
}

static void delay(const struct p101_env *env, struct p101_error *err, time_t min_seconds, time_t max_seconds, long min_nanoseconds, long max_nanoseconds)
{
    struct timespec tim;
    long            random_value;

    if(min_seconds == 0 && max_seconds == 0 && min_nanoseconds == 0 && max_nanoseconds == 0)
    {
        goto done;
    }

    if(min_seconds == max_seconds && min_nanoseconds == max_nanoseconds)
    {
        tim.tv_sec  = min_seconds;
        tim.tv_nsec = min_nanoseconds;
    }
    else
    {
        random_value = generate_random_long(env, min_seconds, max_seconds);
        tim.tv_sec   = random_value;
        random_value = generate_random_long(env, min_nanoseconds, max_nanoseconds);
        tim.tv_nsec  = random_value;
    }

    p101_nanosleep(env, err, &tim, NULL);

done:
    return;
}

static long generate_random_long(const struct p101_env *env, long min, long max)
{
    uintmax_t limit;
    uintmax_t num;
    uintmax_t range;
    uint32_t  random_value;
    long      result;

    if(min == max)
    {
        result = min;
        goto done;
    }

    range = (uintmax_t)(max - min) + UINTMAX_C(1);
    limit = UINTMAX_MAX - (UINTMAX_MAX % range);

    do
    {
        num = 0;

        for(size_t i = 0; i < sizeof(num); i += sizeof(uint32_t))
        {
            random_value = p101_arc4random(env);
            num          = (num << 32) | random_value;    // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
        }
    } while(num >= limit);

    result = min + (long)(num % range);

done:
    return result;
}

void cleanup(const struct p101_env *env, struct p101_error *err, void *arg, struct p101_fsm_effect_sink *sink, struct p101_fsm_decision *decision)
{
    struct server_data *data;

    P101_TRACE_SCOPE(env);
    (void)sink;
    data = (struct server_data *)arg;

    close_socket(env, err, &data->client_socket);
    close_socket(env, err, &data->forward_socket);
    close_socket(env, err, &data->server_socket);

    p101_fsm_decide_exit(decision);
}
