cmake_minimum_required(VERSION 3.10)
project(p2p_av C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FindPkgConfig)

# --- Sources ---
set(P2P_SOURCES
    src/p2p_packet_queue.c
    src/p2p_adapter.c
    src/p2p_signaling.c
    src/p2p_publisher.c
    src/p2p_subscriber.c
    src/p2p_av_capture.c
    src/p2p_av_codec.c
    src/p2p_sdl_render.c
)

set(P2P_HEADERS
    include/p2p_platform.h
    include/p2p_packet_queue.h
    include/p2p_adapter.h
    include/p2p_signaling.h
    include/p2p_publisher.h
    include/p2p_subscriber.h
    include/p2p_av_capture.h
    include/p2p_av_codec.h
    include/p2p_sdl_render.h
)

# --- Library ---
add_library(p2p_av STATIC ${P2P_SOURCES})

target_include_directories(p2p_av
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# --- Dependencies ---

# libjuice
if(TARGET juice)
    target_link_libraries(p2p_av PUBLIC juice)
else()
    find_library(JUICE_LIB juice HINTS ${JUICE_LIB_PATH} ${CMAKE_BINARY_DIR}/libjuice)
    find_path(JUICE_INCLUDE juice/juice.h HINTS ${JUICE_INCLUDE_PATH} ${CMAKE_SOURCE_DIR}/libjuice/include)
    if(JUICE_LIB AND JUICE_INCLUDE)
        target_link_libraries(p2p_av PUBLIC ${JUICE_LIB})
        target_include_directories(p2p_av PUBLIC ${JUICE_INCLUDE})
    else()
        message(FATAL_ERROR "libjuice not found. Set JUICE_LIB_PATH and JUICE_INCLUDE_PATH.")
    endif()
endif()

# xquic - REQUIRED
# Headers from source tree
set(_XQUIC_SRC_DIR "${XQUIC_SRC_DIR}")
if(NOT _XQUIC_SRC_DIR)
    set(_XQUIC_SRC_DIR "${CMAKE_SOURCE_DIR}/xquic")
endif()
if(NOT EXISTS "${_XQUIC_SRC_DIR}/include/xquic/xquic.h")
    message(FATAL_ERROR "xquic headers not found at ${_XQUIC_SRC_DIR}/include")
endif()
target_include_directories(p2p_av PUBLIC "${_XQUIC_SRC_DIR}/include")

# Generated config header from xquic build
set(_XQUIC_BUILD_DIR "${XQUIC_BUILD_DIR}")
if(NOT _XQUIC_BUILD_DIR)
    set(_XQUIC_BUILD_DIR "${CMAKE_BINARY_DIR}/xquic")
endif()
if(EXISTS "${_XQUIC_BUILD_DIR}/include")
    target_include_directories(p2p_av PUBLIC "${_XQUIC_BUILD_DIR}/include")
endif()

# xquic static library
if(TARGET xquic-static)
    target_link_libraries(p2p_av PUBLIC xquic-static)
else()
    find_library(XQUIC_LIB NAMES xquic-static xquic
        HINTS ${XQUIC_LIB_PATH} ${_XQUIC_BUILD_DIR}
        NO_DEFAULT_PATH)
    if(XQUIC_LIB)
        target_link_libraries(p2p_av PUBLIC ${XQUIC_LIB})
    else()
        message(FATAL_ERROR "xquic library not found in ${_XQUIC_BUILD_DIR}. Run scripts/build.sh first.")
    endif()
endif()

# BoringSSL (required by xquic)
set(_BORINGSSL_BUILD_DIR "${BORINGSSL_BUILD_DIR}")
if(NOT _BORINGSSL_BUILD_DIR)
    set(_BORINGSSL_BUILD_DIR "${CMAKE_BINARY_DIR}/boringssl")
endif()
set(_BORINGSSL_SRC_DIR "${BORINGSSL_SRC_DIR}")
if(NOT _BORINGSSL_SRC_DIR)
    set(_BORINGSSL_SRC_DIR "${CMAKE_SOURCE_DIR}/xquic/third_party/boringssl")
endif()

if(WIN32)
    if(EXISTS "${_BORINGSSL_BUILD_DIR}/ssl.lib")
        target_link_libraries(p2p_av PUBLIC
            "${_BORINGSSL_BUILD_DIR}/ssl.lib"
            "${_BORINGSSL_BUILD_DIR}/crypto.lib")
        target_include_directories(p2p_av PUBLIC "${_BORINGSSL_SRC_DIR}/include")
    else()
        find_package(OpenSSL REQUIRED)
        target_link_libraries(p2p_av PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    endif()
else()
    if(EXISTS "${_BORINGSSL_BUILD_DIR}/libssl.a")
        target_link_libraries(p2p_av PUBLIC
            "${_BORINGSSL_BUILD_DIR}/libssl.a"
            "${_BORINGSSL_BUILD_DIR}/libcrypto.a")
        target_include_directories(p2p_av PUBLIC "${_BORINGSSL_SRC_DIR}/include")
    else()
        find_package(OpenSSL REQUIRED)
        target_link_libraries(p2p_av PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    endif()
endif()

# BoringSSL is C++, xquic uses math functions
if(WIN32)
    target_link_libraries(p2p_av PUBLIC ws2_32)
else()
    target_link_libraries(p2p_av PUBLIC stdc++ m)
endif()

# pthreads (not needed on Windows, threading via Win32 API)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(p2p_av PUBLIC Threads::Threads)
endif()

# --- FFmpeg ---
if(WIN32)
    pkg_check_modules(AVCODEC    REQUIRED libavcodec)
    pkg_check_modules(AVUTIL     REQUIRED libavutil)
    pkg_check_modules(SWSCALE    REQUIRED libswscale)
    pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
    pkg_check_modules(AVFORMAT   REQUIRED libavformat)
    pkg_check_modules(AVDEVICE   REQUIRED libavdevice)

    target_include_directories(p2p_av PUBLIC
        ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS}
        ${SWSCALE_INCLUDE_DIRS} ${SWRESAMPLE_INCLUDE_DIRS}
        ${AVFORMAT_INCLUDE_DIRS} ${AVDEVICE_INCLUDE_DIRS})
    target_link_libraries(p2p_av PUBLIC
        ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES}
        ${SWSCALE_LIBRARIES} ${SWRESAMPLE_LIBRARIES}
        ${AVFORMAT_LIBRARIES} ${AVDEVICE_LIBRARIES})
    target_link_directories(p2p_av PUBLIC
        ${AVCODEC_LIBRARY_DIRS} ${AVUTIL_LIBRARY_DIRS}
        ${SWSCALE_LIBRARY_DIRS} ${SWRESAMPLE_LIBRARY_DIRS}
        ${AVFORMAT_LIBRARY_DIRS} ${AVDEVICE_LIBRARY_DIRS})
else()
    pkg_check_modules(AVCODEC    REQUIRED libavcodec)
    pkg_check_modules(AVUTIL     REQUIRED libavutil)
    pkg_check_modules(SWSCALE    REQUIRED libswscale)
    pkg_check_modules(SWRESAMPLE REQUIRED libswresample)

    target_include_directories(p2p_av PUBLIC
        ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS}
        ${SWSCALE_INCLUDE_DIRS} ${SWRESAMPLE_INCLUDE_DIRS})
    target_link_libraries(p2p_av PUBLIC
        ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES}
        ${SWSCALE_LIBRARIES} ${SWRESAMPLE_LIBRARIES})
    target_link_directories(p2p_av PUBLIC
        ${AVCODEC_LIBRARY_DIRS} ${AVUTIL_LIBRARY_DIRS}
        ${SWSCALE_LIBRARY_DIRS} ${SWRESAMPLE_LIBRARY_DIRS})
endif()

# --- SDL2 ---
pkg_check_modules(SDL2 REQUIRED sdl2)
target_include_directories(p2p_av PUBLIC ${SDL2_INCLUDE_DIRS})
target_link_libraries(p2p_av PUBLIC ${SDL2_LIBRARIES})
target_link_directories(p2p_av PUBLIC ${SDL2_LIBRARY_DIRS})

# --- ALSA (Linux only) ---
if(NOT WIN32)
    pkg_check_modules(ALSA REQUIRED alsa)
    target_include_directories(p2p_av PUBLIC ${ALSA_INCLUDE_DIRS})
    target_link_libraries(p2p_av PUBLIC ${ALSA_LIBRARIES})
    target_link_directories(p2p_av PUBLIC ${ALSA_LIBRARY_DIRS})
endif()

# --- Executables ---
add_executable(p2p_client src/p2p_client.c)
target_link_libraries(p2p_client PRIVATE p2p_av)

# p2p_peer is Linux-only (subscriber with SDL2 display)
if(NOT WIN32)
    add_executable(p2p_peer src/p2p_peer.c)
    target_link_libraries(p2p_peer PRIVATE p2p_av)
endif()
