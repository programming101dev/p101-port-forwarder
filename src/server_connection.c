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

    P101_TRACE_SCOPE(env);
    (void)sink;
    p101_printf(env, err, "Handing connection\n");
    data         = (struct server_data *)arg;
    from_started = false;
    to_started   = false;
    cleanup_err  = NULL;

    data->forward_socket = p101_socket(env, err, data->sets->addr_out.ss_family, SOCK_STREAM, 0);

    if(p101_error_has_error(err))
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

    if(p101_error_has_error(err))
    {
        goto error;
    }

    p101_printf(env, err, "Connected to server\n");
    server_active_threads_reset(env);
    from_started = start_copy_thread(env, err, &from_forwarder, &from_data, data->sets, data->forward_socket, data->client_socket);

    if(p101_error_has_error(err))
    {
        goto error;
    }

    to_started = start_copy_thread(env, err, &to_forwarder, &to_data, data->sets, data->client_socket, data->forward_socket);

    if(p101_error_has_error(err))
    {
        goto error;
    }

    // wait for a thread to signal the condition
    p101_pthread_mutex_lock(env, err, server_lock());

    if(p101_error_has_error(err))
    {
        goto error;
    }

    //    p101_printf(env, err, "active threads = %u\n", server_active_threads_load(env));

    while(server_active_threads_load(env) > 0)
    {
        p101_printf(env, err, "active threads = %u\n", server_active_threads_load(env));
        p101_printf(env, err, "waiting on condition\n");
        p101_pthread_cond_wait(env, err, server_cond(), server_lock());
        if(p101_error_has_error(err))
        {
            (void)p101_pthread_mutex_unlock(env, err, server_lock());
            goto error;
        }
        p101_printf(env, err, "condition done\n");
    }

    p101_pthread_mutex_unlock(env, err, server_lock());

    if(p101_error_has_error(err))
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

    if(p101_error_has_error(err))
    {
        goto error;
    }

    close_socket(env, err, &data->client_socket);
    close_socket(env, err, &data->forward_socket);

    if(p101_error_has_error(err))
    {
        goto error;
    }

    if(server_exit_requested())
    {
        next_state = CLEANUP;
    }
    else
    {
        next_state = ACCEPT;
    }
    goto done;

error:
    cleanup_err = p101_error_create(false);
    shutdown_socket(env, cleanup_err == NULL ? err : cleanup_err, data->client_socket, SHUT_RDWR);
    shutdown_socket(env, cleanup_err == NULL ? err : cleanup_err, data->forward_socket, SHUT_RDWR);

    if(from_started)
    {
        p101_pthread_join(env, cleanup_err == NULL ? err : cleanup_err, from_forwarder, NULL);
    }

    if(to_started)
    {
        p101_pthread_join(env, cleanup_err == NULL ? err : cleanup_err, to_forwarder, NULL);
    }

    close_socket(env, cleanup_err == NULL ? err : cleanup_err, &data->client_socket);
    close_socket(env, cleanup_err == NULL ? err : cleanup_err, &data->forward_socket);
    if(cleanup_err != NULL)
    {
        if(p101_error_has_error(cleanup_err))
        {
            p101_fprintf(env, cleanup_err, stderr, "Connection cleanup error: %s\n", p101_error_get_message(cleanup_err));
        }
        p101_error_destroy(cleanup_err);
    }
    if(error_is_connection_local(err) && !server_exit_requested())
    {
        p101_fprintf(env, NULL, stderr, "Connection error: %s\n", p101_error_get_message(err));
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

    started       = false;
    data->env     = env;
    data->sets    = sets;
    data->from_fd = from_socket;
    data->to_fd   = to_socket;

    server_active_threads_increment(env);
    p101_pthread_create(env, err, forwarder_thread, NULL, copy_handler, data);

    if(p101_error_has_error(err))
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
    bool                   closed;

    data = (struct copy_data *)arg;
    env  = data->env;
    err  = p101_error_create(false);

    if(err == NULL)
    {
        /* P101_ERROR_OPTIONAL rationale: the error object allocation itself failed. */
        p101_fprintf(env, P101_ERROR_OPTIONAL, stderr, "Unable to create copy thread error object\n");
        goto done;
    }

    do
    {
        const char *closed_str;

        closed = copy(env, err, data->to_fd, data->from_fd, data->sets);

        if(p101_error_has_error(err))
        {
            p101_fprintf(env, err, stderr, "Copy thread error: %s\n", p101_error_get_message(err));
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
    p101_pthread_mutex_lock(env, err, server_lock());
    p101_pthread_cond_signal(env, err, server_cond());
    p101_pthread_mutex_unlock(env, err, server_lock());
    p101_error_destroy(err);

    return NULL;
}

static bool copy(const struct p101_env *env, struct p101_error *err, int to_fd, int from_fd, const struct settings *sets)
{
    uint8_t buffer[BUFFER_LEN];
    ssize_t bytes_read;
    bool    closed;

    closed = false;
    p101_printf(env, err, "Reading from %d to send to %d\n", from_fd, to_fd);
    bytes_read = p101_read(env, err, from_fd, buffer, BUFFER_LEN);
    p101_printf(env, err, "Read %zd from %d to send to %d\n", bytes_read, from_fd, to_fd);

    if(p101_error_has_error(err))
    {
        if(error_is_retryable(err))
        {
            p101_error_reset(err);
            if(server_exit_requested())
            {
                closed = true;
            }
        }
        else if(error_is_connection_closed(err))
        {
            p101_error_reset(err);
            closed = true;
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
            size_t  bytes_to_write;
            ssize_t bytes_written;

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
                    bytes_to_write = p101_arc4random_uniform(env, sets->max_bytes - sets->min_bytes + 1) + sets->min_bytes;
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

            if(p101_error_has_error(err))
            {
                if(error_is_retryable(err))
                {
                    p101_error_reset(err);
                    if(server_exit_requested())
                    {
                        closed = true;
                        goto done;
                    }
                    continue;
                }

                if(error_is_connection_closed(err))
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
            if(p101_error_has_error(err))
            {
                goto done;
            }

            bytes_remaining -= (size_t)bytes_written;
            pos += (size_t)bytes_written;
            delay(env, err, sets->min_seconds, sets->max_seconds, sets->min_nanoseconds, sets->max_nanoseconds);

            if(p101_error_has_error(err))
            {
                if(error_is_retryable(err) && server_exit_requested())
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
    return copy(env, err, to_fd, from_fd, sets);
}

bool server_connection_error_is_local_for_test(const struct p101_error *err)
{
    return error_is_connection_local(err);
}
#endif

static bool error_is_connection_closed(const struct p101_error *err)
{
    if(p101_error_is_errno(err, EBADF))
    {
        return true;
    }

    if(p101_error_is_errno(err, EPIPE))
    {
        return true;
    }

    if(p101_error_is_errno(err, ECONNRESET))
    {
        return true;
    }

    if(p101_error_is_errno(err, ECONNABORTED))
    {
        return true;
    }

    return false;
}

static bool error_is_connection_local(const struct p101_error *err)
{
    return (p101_error_is_errno(err, ECONNABORTED) || p101_error_is_errno(err, ECONNREFUSED) || p101_error_is_errno(err, ECONNRESET) || p101_error_is_errno(err, EHOSTUNREACH) || p101_error_is_errno(err, ENETDOWN) || p101_error_is_errno(err, ENETUNREACH) ||
            p101_error_is_errno(err, ENOTCONN) || p101_error_is_errno(err, EPIPE) || p101_error_is_errno(err, ETIMEDOUT)) != 0;
}

static bool error_is_retryable(const struct p101_error *err)
{
    if(p101_error_is_errno(err, EINTR))
    {
        return true;
    }

    return false;
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
        tim.tv_sec  = generate_random_long(env, min_seconds, max_seconds);
        tim.tv_nsec = generate_random_long(env, min_nanoseconds, max_nanoseconds);
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

    if(min == max)
    {
        return min;
    }

    range = (uintmax_t)(max - min) + UINTMAX_C(1);
    limit = UINTMAX_MAX - (UINTMAX_MAX % range);

    do
    {
        num = 0;

        for(size_t i = 0; i < sizeof(num); i += sizeof(uint32_t))
        {
            num = (num << 32) | p101_arc4random(env);    // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
        }
    } while(num >= limit);

    return min + (long)(num % range);
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
