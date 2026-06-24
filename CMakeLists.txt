cmake_minimum_required(VERSION 3.20)
project(pcc C)

set(CMAKE_C_STANDARD 11)

# This CMakeLists.txt is for CLion navigation/indexing only.
# The actual build uses meson. Do not use this for production builds.

# DOCA SDK headers
set(DOCA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

include_directories(
    ${DOCA_ROOT}/include
    ${DOCA_ROOT}/applications/common
    ${DOCA_ROOT}/applications/common/device
    ${DOCA_ROOT}/tools/doca_lib_dir/dpa_llvm/lib/clang/18/include
    ${CMAKE_CURRENT_SOURCE_DIR}/host
    ${CMAKE_CURRENT_SOURCE_DIR}/device
    ${CMAKE_CURRENT_SOURCE_DIR}/device/rp/rtt_template/algo
    ${CMAKE_CURRENT_SOURCE_DIR}/device/rp/dcqcn/algo
    ${CMAKE_CURRENT_SOURCE_DIR}/device/rp/switch_telemetry/algo
)

# Host sources
file(GLOB_RECURSE HOST_SOURCES host/*.c host/*.h)

# Device sources
file(GLOB_RECURSE DEVICE_SOURCES device/*.c device/*.h)

# Common application sources
file(GLOB COMMON_SOURCES
    ${DOCA_ROOT}/applications/common/*.c
    ${DOCA_ROOT}/applications/common/*.h
)

# DOCA headers (for indexing)
file(GLOB DOCA_HEADERS ${DOCA_ROOT}/include/*.h)

# Single target so CLion indexes everything together
add_library(pcc_index STATIC EXCLUDE_FROM_ALL
    ${HOST_SOURCES}
    ${DEVICE_SOURCES}
    ${COMMON_SOURCES}
    ${DOCA_HEADERS}
)
