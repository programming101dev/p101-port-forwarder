#include "server_socket.h"
#include "server_state.h"
#include "settings.h"
#include <errno.h>
#include <p101_network/arpa/p101_inet.h>
#include <p101_network/net/p101_ethernet.h>
#include <p101_network/net/p101_if.h>
#include <p101_network/p101_ifaddrs.h>
#include <p101_network/p101_netdb.h>
#include <p101_network/sys/p101_socket.h>
#include <sys/socket.h>

void socket_create(const struct p101_env *env, struct p101_error *err, void *arg, struct p101_fsm_effect_sink *sink, struct p101_fsm_decision *decision)
{
    struct server_data *data;
    p101_fsm_state_id   next_state;
    int                 socket_fd;
    bool                has_error;

    P101_TRACE_SCOPE(env);
    (void)sink;
    data                = (struct server_data *)arg;
    socket_fd           = p101_socket(env, err, data->sets->addr_in.ss_family, SOCK_STREAM, 0);
    data->server_socket = socket_fd;

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        next_state = CLEANUP;
    }
    else
    {
        next_state = BIND;
    }

    p101_fsm_decide_transition(decision, next_state);
}

void socket_bind(const struct p101_env *env, struct p101_error *err, void *arg, struct p101_fsm_effect_sink *sink, struct p101_fsm_decision *decision)
{
    struct server_data *data;
    socklen_t           addr_len;
    in_port_t           net_port;
    p101_fsm_state_id   next_state;
    bool                has_error;

    P101_TRACE_SCOPE(env);
    (void)sink;
    data     = (struct server_data *)arg;
    net_port = p101_htons(env, data->sets->port_in);

    if(data->sets->addr_in.ss_family == AF_INET)
    {
        struct sockaddr_in *ipv4_addr;

        ipv4_addr           = (struct sockaddr_in *)&data->sets->addr_in;
        addr_len            = sizeof(*ipv4_addr);
        ipv4_addr->sin_port = net_port;
    }
    else if(data->sets->addr_in.ss_family == AF_INET6)
    {
        struct sockaddr_in6 *ipv6_addr;

        ipv6_addr            = (struct sockaddr_in6 *)&data->sets->addr_in;
        addr_len             = sizeof(*ipv6_addr);
        ipv6_addr->sin6_port = net_port;
    }
    else
    {
        P101_ERROR_RAISE_USER(err, "Internal error: addr->ss_family must be AF_INET or AF_INET6", 1);
        goto error;
    }

    p101_setsockopt(env, err, data->server_socket, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto error;
    }

    p101_bind(env, err, data->server_socket, (struct sockaddr *)&data->sets->addr_in, addr_len);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto error;
    }

    next_state = LISTEN;
    goto done;

error:
    next_state = CLEANUP;

done:
    p101_fsm_decide_transition(decision, next_state);
}

void socket_listen(const struct p101_env *env, struct p101_error *err, void *arg, struct p101_fsm_effect_sink *sink, struct p101_fsm_decision *decision)
{
    const struct server_data *data;
    p101_fsm_state_id         next_state;
    bool                      has_error;

    P101_TRACE_SCOPE(env);
    (void)sink;
    data = (struct server_data *)arg;
    p101_listen(env, err, data->server_socket, data->sets->backlog);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        next_state = CLEANUP;
    }
    else
    {
        next_state = ACCEPT;
    }

    p101_fsm_decide_transition(decision, next_state);
}

void socket_accept(const struct p101_env *env, struct p101_error *err, void *arg, struct p101_fsm_effect_sink *sink, struct p101_fsm_decision *decision)
{
    struct p101_error  *accept_err;
    struct server_data *data;
    p101_fsm_state_id   next_state;
    bool                exit_requested;
    bool                is_interrupted;
    bool                no_error;
    bool                has_error;
    bool                retry;
    int                 client_socket;

    P101_TRACE_SCOPE(env);
    (void)sink;
    accept_err = NULL;
    data       = (struct server_data *)arg;

    exit_requested = server_exit_requested();
    if(exit_requested)
    {
        next_state = CLEANUP;
        goto done;
    }

    accept_err = p101_error_create(false);

    if(accept_err == NULL)
    {
        P101_ERROR_RAISE_SYSTEM(err, "Unable to create accept error object", 1);
        next_state = CLEANUP;
        goto done;
    }

    retry = true;
    while(retry)
    {
        client_socket       = p101_accept(env, accept_err, data->server_socket, NULL, NULL);
        data->client_socket = client_socket;
        is_interrupted      = p101_error_is_errno(accept_err, EINTR);
        exit_requested      = server_exit_requested();

        if(is_interrupted && !exit_requested)
        {
            p101_error_reset(accept_err);
        }
        no_error = p101_error_has_no_error(accept_err);
        retry    = (no_error && data->client_socket == -1 && !exit_requested) != 0;
    }

    has_error = p101_error_has_error(accept_err);
    if(has_error)
    {
        is_interrupted = p101_error_is_errno(accept_err, EINTR);
        exit_requested = server_exit_requested();
        if(is_interrupted && exit_requested)
        {
            p101_error_reset(accept_err);
        }
        else
        {
            p101_error_move(err, accept_err);
        }

        next_state = CLEANUP;
    }
    else
    {
        exit_requested = server_exit_requested();
        if(exit_requested)
        {
            next_state = CLEANUP;
        }
        else
        {
            next_state = HANDLE;
        }
    }

done:
    p101_error_destroy(accept_err);

    p101_fsm_decide_transition(decision, next_state);
}
