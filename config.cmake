set(PROJECT_NAME "p101-port-forwarder")
set(PROJECT_VERSION "1.0.0")
set(PROJECT_DESCRIPTION "Simple Port Forwarder")
set(PROJECT_LANGUAGE "C")

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Common compiler flags
set(STANDARD_FLAGS
        -D_POSIX_C_SOURCE=200809L
        -D_XOPEN_SOURCE=700
        -Werror
)

set(DARWIN_STANDARD_FLAGS
        -D_DARWIN_C_SOURCE
)

set(LINUX_STANDARD_FLAGS
)

set(BSD_STANDARD_FLAGS
)

# Define targets
set(EXECUTABLE_TARGETS p101_port_forwarder)
set(p101_port_forwarder_OUTPUT_NAME p101-port-forwarder)
set(LIBRARY_TARGETS "")

set(p101_port_forwarder_SOURCES
        src/cli.c
        src/main.c
        src/convert.c
        src/server.c
        src/server_connection.c
        src/server_settings.c
        src/server_signal.c
        src/server_socket.c
        src/server_state.c
)

set(p101_port_forwarder_HEADERS
        include/cli.h
        include/convert.h
        include/server.h
        include/settings.h
        include/server_connection.h
        include/server_settings.h
        include/server_signal.h
        include/server_socket.h
        include/server_state.h
)

set(p101_port_forwarder_LINK_LIBRARIES
        p101_error
        p101_env
        p101_c
        p101_convert
        p101_cli
        p101_filesystem
        p101_io
        p101_network
        p101_process
        p101_random
        p101_sync
        p101_thread
        p101_time
        p101_fsm
        m
        iconv
        )
