# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.20)
project(dnuma VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# --------------------------------------------------------------------------
# Kernel UAPI headers.
#
# dnuma uses userfaultfd interfaces (UFFDIO_MOVE, the RWP definitions, ...)
# that only exist in recent kernels' UAPI headers; distribution headers are
# usually far older than that. Point DNUMA_KERNEL_HEADERS at the `include`
# directory produced by `make headers_install` of the target kernel
# (scripts/kernel-headers.sh fetches and installs them; the reference target
# is Linux 7.3.5). The binaries still run on older kernels: every feature is
# detected at run time.
# --------------------------------------------------------------------------
set(DNUMA_KERNEL_HEADERS "" CACHE PATH "Directory with installed kernel UAPI headers (.../include)")
if(DNUMA_KERNEL_HEADERS)
  if(NOT EXISTS "${DNUMA_KERNEL_HEADERS}/linux/userfaultfd.h")
    message(FATAL_ERROR "DNUMA_KERNEL_HEADERS=${DNUMA_KERNEL_HEADERS} has no linux/userfaultfd.h")
  endif()
  include_directories(BEFORE SYSTEM "${DNUMA_KERNEL_HEADERS}")
  set(CMAKE_REQUIRED_INCLUDES "${DNUMA_KERNEL_HEADERS}")
  if(EXISTS "${DNUMA_KERNEL_HEADERS}/linux/version.h")
    file(STRINGS "${DNUMA_KERNEL_HEADERS}/linux/version.h" _kver REGEX "LINUX_VERSION_(MAJOR|PATCHLEVEL|SUBLEVEL)")
    string(REGEX REPLACE ".*MAJOR ([0-9]+).*PATCHLEVEL ([0-9]+).*SUBLEVEL ([0-9]+).*" "\\1.\\2.\\3" DNUMA_UAPI_VERSION "${_kver}")
  endif()
endif()
if(NOT DNUMA_UAPI_VERSION)
  set(DNUMA_UAPI_VERSION "system")
endif()
include(CheckSymbolExists)
check_symbol_exists(UFFDIO_MOVE "linux/userfaultfd.h" DNUMA_HAVE_UFFDIO_MOVE)
check_symbol_exists(UFFDIO_RWPROTECT "linux/userfaultfd.h" DNUMA_HAVE_UFFDIO_RWPROTECT)
if(NOT DNUMA_HAVE_UFFDIO_MOVE OR NOT DNUMA_HAVE_UFFDIO_RWPROTECT)
  message(FATAL_ERROR
    "The kernel UAPI headers in use do not define UFFDIO_MOVE / UFFDIO_RWPROTECT.\n"
    "Run scripts/kernel-headers.sh and configure with -DDNUMA_KERNEL_HEADERS=<dir>/include "
    "(reference target: Linux 7.3.5).")
endif()
message(STATUS "dnuma: building against kernel UAPI headers ${DNUMA_UAPI_VERSION}")
add_compile_definitions(DNUMA_UAPI_VERSION="${DNUMA_UAPI_VERSION}" DNUMA_VERSION="${PROJECT_VERSION}")

# Sanitizer builds of the daemons and unit tests (not of the static,
# fixed-layout distributed programs): -DDNUMA_SANITIZE=address
set(DNUMA_SANITIZE "" CACHE STRING "Sanitizer for daemons and unit tests (address, undefined, ...)")
if(DNUMA_SANITIZE)
  add_compile_options(-fsanitize=${DNUMA_SANITIZE} -O1)
  add_link_options(-fsanitize=${DNUMA_SANITIZE})
  # The daemons are built on one machine and run on others, which need not
  # have the same compiler's sanitizer runtime installed: link it in.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(DNUMA_SANITIZE MATCHES "address")
      add_link_options(-static-libasan)
    endif()
    if(DNUMA_SANITIZE MATCHES "undefined")
      add_link_options(-static-libubsan)
    endif()
  endif()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(IBVERBS REQUIRED IMPORTED_TARGET libibverbs)
pkg_check_modules(RDMACM REQUIRED IMPORTED_TARGET librdmacm)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Werror -Wno-missing-field-initializers -Wno-unused-parameter
                    -fno-omit-frame-pointer)
# GCC 11 reports false positives for memcpy into std::vector storage at -O2
# (PR 100366 and friends); keep them visible but not fatal.
add_compile_options(-Wno-error=array-bounds -Wno-error=stringop-overflow -Wno-error=stringop-overread)
include_directories(${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)

# ---- core: no RDMA dependency; also linked into distributed applications ----
add_library(dnuma_core STATIC
  src/common/util.cc src/common/log.cc src/common/sha256.cc src/common/reactor.cc
  src/common/metrics.cc src/common/hwprobe.cc
  src/transport/stream.cc src/transport/net.cc
  src/export/sysdesc.cc src/export/ptrace_util.cc)
target_link_libraries(dnuma_core PUBLIC Threads::Threads)

# ---- RDMA transport + node-side memory manager ----
add_library(dnuma_node STATIC
  src/transport/rdma.cc
  src/node/page_pool.cc src/node/link_table.cc src/node/node_core.cc)
target_link_libraries(dnuma_node PUBLIC dnuma_core PkgConfig::IBVERBS PkgConfig::RDMACM)

# ---- master-side pure logic (unit-testable without hardware) ----
add_library(dnuma_coord STATIC
  src/master/coherence.cc src/master/lifecycle.cc src/master/topology.cc src/master/kernel_view.cc)
target_link_libraries(dnuma_coord PUBLIC dnuma_core)

if(EXISTS ${CMAKE_SOURCE_DIR}/src/master/main.cc)
  add_executable(dnuma-master
    src/master/main.cc src/master/master.cc src/master/master_slaves.cc
    src/master/master_proc.cc src/master/master_ctl.cc src/master/master_export.cc src/master/export.cc
    src/master/export_shards.cc src/master/master_view.cc)
  target_link_libraries(dnuma-master PRIVATE dnuma_coord dnuma_node)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/src/agent/main.cc)
  add_executable(dnuma-agent src/agent/main.cc src/agent/agent.cc src/agent/body.cc)
  target_link_libraries(dnuma-agent PRIVATE dnuma_node)

  # The body stub: freestanding, static, at a fixed out-of-the-way address so
  # that it does not collide with the mappings of the process it will host.
  add_executable(dnuma-body src/agent/body_stub.c)
  set_target_properties(dnuma-body PROPERTIES COMPILE_OPTIONS "" LINK_OPTIONS "")
  # (position-independent code: the link address is above 2 GiB, so absolute
  #  32-bit references would not reach)
  target_compile_options(dnuma-body PRIVATE -O2 -ffreestanding -fno-stack-protector -fPIE -fcf-protection=none
                         -fno-asynchronous-unwind-tables -fno-sanitize=all)
  target_link_options(dnuma-body PRIVATE -nostdlib -static -no-pie -fno-sanitize=all
                      -Wl,-Ttext-segment=0x6e756d610000 -Wl,--build-id=none -Wl,-z,noexecstack)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/src/cli/dnumactl.cc)
  add_executable(dnumactl src/cli/dnumactl.cc)
  target_link_libraries(dnumactl PRIVATE dnuma_core)
  add_executable(dnuma-run src/cli/dnuma_run.cc)
  target_link_libraries(dnuma-run PRIVATE dnuma_core)
endif()

# ---- libdnuma: the application runtime ----
# Distributed programs are linked statically and non-PIE so that code and
# static data have identical addresses in every image on every machine.
if(EXISTS ${CMAKE_SOURCE_DIR}/src/runtime/runtime.cc AND NOT DNUMA_SANITIZE)
  add_library(dnuma STATIC src/runtime/runtime.cc src/runtime/ctx_switch.S)
  target_compile_options(dnuma PRIVATE -fno-stack-protector)
  target_link_libraries(dnuma PUBLIC dnuma_core)
  function(dnuma_add_executable name)
    add_executable(${name} ${ARGN})
    target_compile_options(${name} PRIVATE -fno-stack-protector -fno-pie)
    target_link_libraries(${name} PRIVATE dnuma)
    target_link_options(${name} PRIVATE -static -no-pie -Wl,--wrap=main)
  endfunction()
  add_subdirectory(examples)
endif()

enable_testing()
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt)
  add_subdirectory(tests)
endif()
