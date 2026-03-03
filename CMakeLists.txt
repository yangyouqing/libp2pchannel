cmake_minimum_required(VERSION 3.10)
project(libp2pchannel C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# === Options ===
option(BUILD_LIBJUICE "Build libjuice from source" ON)
option(BUILD_COTURN   "Build coturn from source" OFF)
option(BUILD_TESTS    "Build end-to-end test programs" ON)

# === Pre-built dependency paths (all under build/) ===
# BoringSSL and xquic are built separately before this cmake runs.
# These variables tell p2p/CMakeLists.txt where to find them.
set(BORINGSSL_BUILD_DIR "${CMAKE_BINARY_DIR}/boringssl" CACHE PATH
    "Path to BoringSSL build directory containing libssl.a and libcrypto.a")
set(BORINGSSL_SRC_DIR "${CMAKE_SOURCE_DIR}/third_party/xquic/third_party/boringssl" CACHE PATH
    "Path to BoringSSL source directory for headers")
set(XQUIC_BUILD_DIR "${CMAKE_BINARY_DIR}/xquic" CACHE PATH
    "Path to xquic build directory containing libxquic-static.a")
set(XQUIC_SRC_DIR "${CMAKE_SOURCE_DIR}/third_party/xquic" CACHE PATH
    "Path to xquic source directory for headers")

# === libjuice ===
if(BUILD_LIBJUICE)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/libjuice)
endif()

# === coturn ===
if(BUILD_COTURN)
    add_subdirectory(coturn)
endif()

# === P2P adapter library ===
add_subdirectory(p2p)

# === Test programs ===
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
