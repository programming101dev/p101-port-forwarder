# p101-port-forwarder Repository Guide

Welcome to the `p101-port-forwarder` repository — a small TCP port forwarder built on the p101 libraries. This guide will help you set up, build, and run the program.

## **Table of Contents**

1. [Cloning the Repository](#cloning-the-repository)
2. [Prerequisites](#prerequisites)
3. [Configuring the Build](#configuring-the-build)
4. [Building](#building)
5. [Testing](#testing)
6. [Running](#running)
7. [Adding or Removing Files](#adding-or-removing-files)

## **Cloning the Repository**

Clone the repository using the following command:

```bash
git clone https://github.com/programming101dev/p101-port-forwarder.git
```

Navigate to the cloned directory:

```bash
cd p101-port-forwarder
```

Ensure the scripts are executable:

```bash
chmod +x *.sh
```

## **Prerequisites**

The p101 libraries must be installed first (clone the [scripts](https://github.com/programming101dev/scripts) repository and run its `setup.sh`). Then, to ensure you have all of the required tools installed, run:

```bash
./check-env.sh
```

If you are missing tools follow these [instructions](https://docs.google.com/document/d/1ZPqlPD1mie5iwJ2XAcNGz7WeA86dTLerFXs9sAuwCco/edit?usp=drive_link). If something still looks wrong, `./doctor.sh` reports what actually works on this machine for this project.

## **Configuring the Build**

Tell CMake which compiler you want to use:

```bash
./change-compiler.sh -c <compiler>
```

To see the list of possible compilers:

```bash
cat supported_c_compilers.txt
```

Run it again any time to switch compilers; each compiler configures into its own build directory (e.g. `build-clang`, `build-gcc-15`).

## **Building**

To build the program run:

```bash
./build.sh
```

This compiles through the strict analysis pipeline: the clang-format check, clang-tidy, cppcheck, the Clang static analyzer, and hundreds of warnings under `-Werror`. `./build.sh -f` applies the formatter and tidy fixes in place.

## **Testing**

`./check.sh` runs the pre-submit gate: the format check, the strict build, and
the tests, with a single PASS/FAIL at the end. The native test tree exercises
byte-preserving forwarding under deliberately short chunks, half-close/EOF
handling, and settings rejection. Run it directly with `./test.sh`.

The test boundary is the forwarding engine itself. It uses local `socketpair`
endpoints, so it neither depends on the external network nor claims to test
router, firewall, DNS, or third-party TCP behavior.

## **Running**

The binary lands in the configured build directory (e.g. `build-clang/p101-port-forwarder`). Run it with no arguments to see the usage message listing the required listening/forwarding addresses, ports, and backlog.

The process exit status is zero after a clean forwarding session and non-zero
when argument validation, setup, forwarding, or cleanup reports an error. The
program cannot prove anything about traffic that never reaches its admitted
listening socket.

## **Adding or Removing Files**

The `CMakeLists.txt` is fixed and shared across every repository — do not edit it. When you add or remove a source or header, edit the `p101_port_forwarder_*` lists in `config.cmake`, then re-configure and build:

```bash
./change-compiler.sh -c <compiler>
./build.sh
```
