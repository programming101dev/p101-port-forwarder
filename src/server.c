#include "server.h"
#include "convert.h"
#include "server_connection.h"
#include "server_settings.h"
#include "server_signal.h"
#include "server_socket.h"
#include "server_state.h"
#include <arpa/inet.h>
#include <p101_c/p101_stdio.h>
#include <p101_fsm/fsm.h>

void run_server(const struct p101_env *env, struct p101_error *err, struct settings *sets)
{
    char                                    ip_in_str[INET6_ADDRSTRLEN];
    char                                    ip_out_str[INET6_ADDRSTRLEN];
    struct p101_error                      *fsm_err;
    struct p101_env                        *fsm_env;
    struct p101_fsm_info                   *fsm;
    p101_fsm_run_result                     fsm_result;
    struct p101_fsm_step_result             last_step;
    static const struct p101_fsm_transition transitions[] = {
        {P101_FSM_INIT, SOCKET,  socket_create    },
        {SOCKET,        BIND,    socket_bind      },
        {SOCKET,        CLEANUP, cleanup          },
        {BIND,          LISTEN,  socket_listen    },
        {BIND,          CLEANUP, cleanup          },
        {LISTEN,        ACCEPT,  socket_accept    },
        {LISTEN,        CLEANUP, cleanup          },
        {ACCEPT,        HANDLE,  handle_connection},
        {ACCEPT,        CLEANUP, cleanup          },
        {HANDLE,        ACCEPT,  socket_accept    },
        {HANDLE,        CLEANUP, cleanup          }
    };
    struct server_data data;

    P101_TRACE_SCOPE(env);
    fsm_err = NULL;
    fsm_env = NULL;
    fsm     = NULL;

    check_settings(env, err, sets);

    if(p101_error_has_error(err))
    {
        goto error;
    }

    sockaddr_to_string(env, err, &sets->addr_in, ip_in_str, INET6_ADDRSTRLEN);

    if(p101_error_has_error(err))
    {
        goto error;
    }

    sockaddr_to_string(env, err, &sets->addr_out, ip_out_str, INET6_ADDRSTRLEN);

    if(p101_error_has_error(err))
    {
        goto error;
    }

    p101_printf(env, err, "Starting port forwarder %s:%d -> %s:%d\n", ip_in_str, sets->port_in, ip_out_str, sets->port_out);
    setup_signal_handler(env, err);

    if(p101_error_has_error(err))
    {
        goto error;
    }

    fsm_err = p101_error_create(false);
    fsm_env = p101_env_create(fsm_err, NULL);

    if(p101_error_has_error(fsm_err))
    {
        P101_ERROR_RAISE_USER(err, p101_error_get_message(fsm_err), 1);
        goto error;
    }

    if(sets->very_verbose)
    {
        p101_env_set_tracer(fsm_env, p101_env_default_tracer);
    }

    fsm = p101_fsm_info_create(env, err, "port-forwarder", fsm_env, fsm_err, transitions, sizeof(transitions) / sizeof(transitions[0]), NULL);

    if(p101_error_has_error(fsm_err))
    {
        goto error;
    }

    if(p101_error_has_error(err))
    {
        goto error;
    }

    if(sets->very_verbose)
    {
        p101_fsm_info_set_bad_change_state_notifier(env, fsm, p101_fsm_info_default_bad_change_state_notifier);
        p101_fsm_info_set_will_change_state_notifier(env, fsm, p101_fsm_info_default_will_change_state_notifier);
        p101_fsm_info_set_did_change_state_notifier(env, fsm, p101_fsm_info_default_did_change_state_notifier);
    }

    data.sets           = sets;
    data.server_socket  = -1;
    data.client_socket  = -1;
    data.forward_socket = -1;

    fsm_result = p101_fsm_run(fsm, &data, NULL, &last_step);
    if(fsm_result != P101_FSM_RUN_EXITED && p101_error_has_no_error(err) && p101_error_has_no_error(fsm_err))
    {
        P101_ERROR_RAISE_USER(err, "Port-forwarder FSM stopped before exit", 1);
        goto error;
    }

    if(p101_error_has_error(fsm_err))
    {
        goto error;
    }

    if(p101_error_has_error(err))
    {
        goto error;
    }

    goto done;

error:
    if(p101_error_has_no_error(err) && p101_error_has_error(fsm_err))
    {
        P101_ERROR_RAISE_USER(err, p101_error_get_message(fsm_err), 1);
    }

done:
    p101_fsm_info_destroy(env, fsm_err, &fsm);
    p101_env_destroy(fsm_env);
    p101_error_destroy(fsm_err);
}
