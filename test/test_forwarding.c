#include "convert.h"
#include "server_connection.h"
#include "server_settings.h"
#include "settings.h"
#include <p101_env/env.h>
#include <p101_error/error.h>
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
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static int failures;

static void check(bool condition, const char *message)
{
    if(!condition)
    {
        (void)fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static struct settings default_settings(void)
{
    struct settings sets = {0};

    sets.backlog   = 1;
    sets.min_bytes = 3;
    sets.max_bytes = 3;
    return sets;
}

static void close_pair(const struct p101_env *env, struct p101_error *err, int pair[2])
{
    for(size_t index = 0; index < 2U; index++)
    {
        if(pair[index] >= 0)
        {
            p101_close(env, err, pair[index]);
            pair[index] = -1;
            if(p101_error_has_error(err))
            {
                p101_error_reset(err);
            }
        }
    }
}

static bool send_all(const struct p101_env *env, struct p101_error *err, int socket, const char *buffer, size_t length)
{
    size_t offset = 0;
    bool   sent   = false;

    while(offset < length)
    {
        ssize_t count = p101_send(env, err, socket, buffer + offset, length - offset, 0);

        if(count <= 0 || p101_error_has_error(err))
        {
            goto done;
        }
        offset += (size_t)count;
    }
    sent = true;

done:
    return sent;
}

static bool receive_all(const struct p101_env *env, struct p101_error *err, int socket, char *buffer, size_t length)
{
    size_t offset   = 0;
    bool   received = false;

    while(offset < length)
    {
        ssize_t count = p101_recv(env, err, socket, buffer + offset, length - offset, 0);

        if(count <= 0 || p101_error_has_error(err))
        {
            goto done;
        }
        offset += (size_t)count;
    }
    received = true;

done:
    return received;
}

static void test_copy_preserves_all_bytes(const struct p101_env *env, struct p101_error *err)
{
    static const char message[]                 = "short writes must not lose bytes";
    char              received[sizeof(message)] = {0};
    int               input[2]                  = {-1, -1};
    int               output[2]                 = {-1, -1};
    struct settings   sets                      = default_settings();
    bool              closed;

    p101_socketpair(env, err, AF_UNIX, SOCK_STREAM, 0, input);
    check(p101_error_has_no_error(err), "create input socket pair");
    if(p101_error_has_error(err))
    {
        goto done;
    }

    p101_socketpair(env, err, AF_UNIX, SOCK_STREAM, 0, output);
    check(p101_error_has_no_error(err), "create output socket pair");
    if(p101_error_has_error(err))
    {
        goto done;
    }

    check(send_all(env, err, input[0], message, sizeof(message)), "write complete input payload");
    check(p101_error_has_no_error(err), "input write has no error");
    if(p101_error_has_error(err))
    {
        goto done;
    }

    closed = server_copy_once_for_test(env, err, output[0], input[1], &sets);
    check(!closed, "data does not look like end-of-stream");
    check(p101_error_has_no_error(err), "copy has no error");
    if(p101_error_has_error(err))
    {
        goto done;
    }

    check(receive_all(env, err, output[1], received, sizeof(received)), "receive complete forwarded payload");
    check(memcmp(received, message, sizeof(message)) == 0, "forwarded bytes are unchanged");

done:
    if(p101_error_has_error(err))
    {
        p101_error_reset(err);
    }
    close_pair(env, err, input);
    close_pair(env, err, output);
}

static void test_copy_reports_half_close(const struct p101_env *env, struct p101_error *err)
{
    int             input[2]  = {-1, -1};
    int             output[2] = {-1, -1};
    struct settings sets      = default_settings();
    bool            closed;

    p101_socketpair(env, err, AF_UNIX, SOCK_STREAM, 0, input);
    p101_socketpair(env, err, AF_UNIX, SOCK_STREAM, 0, output);
    check(p101_error_has_no_error(err), "create half-close socket pairs");
    if(p101_error_has_error(err))
    {
        goto done;
    }

    p101_shutdown(env, err, input[0], SHUT_WR);
    check(p101_error_has_no_error(err), "half-close input writer");
    if(p101_error_has_error(err))
    {
        goto done;
    }

    closed = server_copy_once_for_test(env, err, output[0], input[1], &sets);
    check(closed, "EOF is reported as closed");
    check(p101_error_has_no_error(err), "EOF is not an error");

done:
    if(p101_error_has_error(err))
    {
        p101_error_reset(err);
    }
    close_pair(env, err, input);
    close_pair(env, err, output);
}

static void test_settings_contract(const struct p101_env *env, struct p101_error *err)
{
    struct settings sets = default_settings();

    check_settings(env, err, &sets);
    check(p101_error_has_no_error(err), "valid settings accepted");

    sets.min_bytes = 4;
    sets.max_bytes = 3;
    check_settings(env, err, &sets);
    check(p101_error_has_error(err), "reversed byte bounds rejected");
    p101_error_reset(err);

    sets             = default_settings();
    sets.min_seconds = -1;
    sets.max_seconds = -1;
    check_settings(env, err, &sets);
    check(p101_error_has_error(err), "negative delay rejected");
    p101_error_reset(err);

    sets                 = default_settings();
    sets.max_nanoseconds = 1000000000L;
    check_settings(env, err, &sets);
    check(p101_error_has_error(err), "out-of-range nanoseconds rejected");
    p101_error_reset(err);
}

static void test_connection_error_routing(struct p101_error *err)
{
    P101_ERROR_RAISE_ERRNO(err, ECONNREFUSED);
    check(server_connection_error_is_local_for_test(err), "connection refusal remains local to one client");
    p101_error_reset(err);

    P101_ERROR_RAISE_ERRNO(err, ETIMEDOUT);
    check(server_connection_error_is_local_for_test(err), "connection timeout remains local to one client");
    p101_error_reset(err);

    P101_ERROR_RAISE_ERRNO(err, ENOMEM);
    check(!server_connection_error_is_local_for_test(err), "process resource failure stops the server");
    p101_error_reset(err);
}

static void test_address_conversion_falls_through_to_ipv6(const struct p101_env *env, struct p101_error *err)
{
    struct sockaddr_storage address;

    convert_address(env, err, "::1", &address);
    check(p101_error_has_no_error(err), "valid IPv6 address is accepted after the IPv4 probe");
    check(address.ss_family == AF_INET6, "valid IPv6 address selects AF_INET6");

    convert_address(env, err, "not-an-address", &address);
    check(p101_error_has_error(err), "invalid address is rejected");
    p101_error_reset(err);
}

int main(void)
{
    struct p101_error *err    = NULL;
    struct p101_env   *env    = NULL;
    int                status = 1;

    failures = 0;
    err      = p101_error_create(false);
    if(err != NULL)
    {
        env = p101_env_create(err, NULL);
    }
    if(err == NULL || env == NULL || p101_error_has_error(err))
    {
        (void)fprintf(stderr, "FAIL: unable to create p101 test context\n");
        status = 1;
        goto done;
    }

    test_copy_preserves_all_bytes(env, err);
    test_copy_reports_half_close(env, err);
    test_settings_contract(env, err);
    test_connection_error_routing(err);
    test_address_conversion_falls_through_to_ipv6(env, err);
    status = failures == 0 ? 0 : 1;

done:
    p101_env_destroy(env);
    p101_error_destroy(err);
    if(status == 0)
    {
        (void)puts("p101-port-forwarder tests passed");
    }
    return status;
}
