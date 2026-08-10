/*
 * This code is licensed under the Attribution-NonCommercial-NoDerivatives 4.0
 * International license.
 *
 * Author: D'Arcy Smith (ds@programming101.dev)
 */

#include "cli.h"
#include "server.h"
#include <p101_c/p101_stdio.h>
#include <p101_c/p101_stdlib.h>
#include <p101_c/p101_string.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    struct p101_error *err;
    struct p101_env   *env;
    struct arguments   args;
    struct settings    sets;
    int                exit_code;
    bool               has_error;
    bool               no_error;
    const char        *message;

    err = p101_error_create(true);
    env = p101_env_create(err, NULL);
    p101_memset(env, &args, 0, sizeof(args));
    parse_arguments(env, err, argc, argv, &args);

    no_error = p101_error_has_no_error(err);
    if(args.show_help && no_error)
    {
        usage(env, err, argv[0], EXIT_SUCCESS, NULL);
        exit_code = EXIT_SUCCESS;
        goto done;
    }

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        message = p101_error_get_message(err);
        usage(env, err, argv[0], EXIT_FAILURE, message);
        exit_code = EXIT_FAILURE;
        goto done;
    }

    if(args.verbose || args.very_verbose)
    {
        p101_env_set_tracer(env, p101_env_default_tracer);
    }

    check_arguments(env, err, &args);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        message = p101_error_get_message(err);
        usage(env, err, argv[0], EXIT_FAILURE, message);
        exit_code = EXIT_FAILURE;
        goto done;
    }

    p101_memset(env, &sets, 0, sizeof(sets));
    convert_arguments(env, err, &args, &sets);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto error;
    }

    run_server(env, err, &sets);

    has_error = p101_error_has_error(err);
    if(has_error)
    {
        goto error;
    }

    exit_code = EXIT_SUCCESS;
    goto done;

error:
    message = p101_error_get_message(err);
    p101_fprintf(env, err, stderr, "Error: %s\n", message);
    exit_code = EXIT_FAILURE;

done:
    p101_env_destroy(env);
    p101_error_destroy(err);

    return exit_code;
}
