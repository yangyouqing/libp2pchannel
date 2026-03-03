cmake_minimum_required(VERSION 3.10)
project(p2p_av VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FindPkgConfig)

# --- Dependency discovery (used by both libp2pav and executables) ---

# libjuice
if(NOT TARGET juice)
    find_library(JUICE_LIB juice HINTS ${JUICE_LIB_PATH} ${CMAKE_BINARY_DIR}/third_party/libjuice)
    find_path(JUICE_INCLUDE juice/juice.h HINTS ${JUICE_INCLUDE_PATH} ${CMAKE_SOURCE_DIR}/third_party/libjuice/include)
    if(NOT JUICE_LIB OR NOT JUICE_INCLUDE)
        message(FATAL_ERROR "libjuice not found. Set JUICE_LIB_PATH and JUICE_INCLUDE_PATH.")
    endif()
endif()

# xquic - REQUIRED
set(_XQUIC_SRC_DIR "${XQUIC_SRC_DIR}")
if(NOT _XQUIC_SRC_DIR)
    set(_XQUIC_SRC_DIR "${CMAKE_SOURCE_DIR}/third_party/xquic")
endif()
if(NOT EXISTS "${_XQUIC_SRC_DIR}/include/xquic/xquic.h")
    message(FATAL_ERROR "xquic headers not found at ${_XQUIC_SRC_DIR}/include")
endif()

set(_XQUIC_BUILD_DIR "${XQUIC_BUILD_DIR}")
if(NOT _XQUIC_BUILD_DIR)
    set(_XQUIC_BUILD_DIR "${CMAKE_BINARY_DIR}/xquic")
endif()

if(NOT TARGET xquic-static)
    find_library(XQUIC_LIB NAMES xquic-static xquic
        HINTS ${XQUIC_LIB_PATH} ${_XQUIC_BUILD_DIR}
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    if(NOT XQUIC_LIB)
        message(FATAL_ERROR "xquic library not found in ${_XQUIC_BUILD_DIR}. Run scripts/build.sh first.")
    endif()
endif()

# TLS library: mbedTLS or BoringSSL (required by xquic)
set(_MBEDTLS_BUILD_DIR "${MBEDTLS_BUILD_DIR}")
if(NOT _MBEDTLS_BUILD_DIR)
    set(_MBEDTLS_BUILD_DIR "${CMAKE_BINARY_DIR}/mbedtls")
endif()
set(_MBEDTLS_SRC_DIR "${MBEDTLS_SRC_DIR}")
if(NOT _MBEDTLS_SRC_DIR)
    set(_MBEDTLS_SRC_DIR "${CMAKE_SOURCE_DIR}/third_party/mbedtls")
endif()

set(_BORINGSSL_BUILD_DIR "${BORINGSSL_BUILD_DIR}")
if(NOT _BORINGSSL_BUILD_DIR)
    set(_BORINGSSL_BUILD_DIR "${CMAKE_BINARY_DIR}/boringssl")
endif()
set(_BORINGSSL_SRC_DIR "${BORINGSSL_SRC_DIR}")
if(NOT _BORINGSSL_SRC_DIR)
    set(_BORINGSSL_SRC_DIR "${CMAKE_SOURCE_DIR}/third_party/xquic/third_party/boringssl")
endif()

# pthreads (not needed on Windows)
if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

# --- libp2pav: transport-only shared library (no FFmpeg/SDL/ALSA deps) ---
set(P2PAV_TRANSPORT_SOURCES
    src/p2p_packet_queue.c
    src/p2p_adapter.c
    src/p2p_tls.c
    src/p2p_signaling.c
    src/p2pav.c
)

add_library(p2pav SHARED ${P2PAV_TRANSPORT_SOURCES})
target_compile_definitions(p2pav PRIVATE P2PAV_BUILDING_DLL)
set_target_properties(p2pav PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION 1
    OUTPUT_NAME p2pav
)
target_include_directories(p2pav PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# libjuice (PUBLIC: p2p_adapter.h exposes juice types)
if(TARGET juice)
    target_link_libraries(p2pav PUBLIC juice)
else()
    if(JUICE_LIB AND JUICE_INCLUDE)
        target_link_libraries(p2pav PUBLIC ${JUICE_LIB})
        target_include_directories(p2pav PUBLIC ${JUICE_INCLUDE})
    endif()
endif()

# xquic
target_include_directories(p2pav PUBLIC "${_XQUIC_SRC_DIR}/include")
if(EXISTS "${_XQUIC_BUILD_DIR}/include")
    target_include_directories(p2pav PUBLIC "${_XQUIC_BUILD_DIR}/include")
endif()
if(TARGET xquic-static)
    target_link_libraries(p2pav PUBLIC xquic-static)
elseif(XQUIC_LIB)
    target_link_libraries(p2pav PUBLIC ${XQUIC_LIB})
endif()

# TLS library
if(EXISTS "${_MBEDTLS_BUILD_DIR}/library/libmbedtls.a")
    target_compile_definitions(p2pav PRIVATE XQC_USE_MBEDTLS=1)
    target_link_libraries(p2pav PRIVATE
        "${_MBEDTLS_BUILD_DIR}/library/libmbedtls.a"
        "${_MBEDTLS_BUILD_DIR}/library/libmbedx509.a"
        "${_MBEDTLS_BUILD_DIR}/library/libmbedcrypto.a")
    target_include_directories(p2pav PRIVATE "${_MBEDTLS_SRC_DIR}/include")
elseif(EXISTS "${_BORINGSSL_BUILD_DIR}/libssl.a")
    target_link_libraries(p2pav PRIVATE
        "${_BORINGSSL_BUILD_DIR}/libssl.a"
        "${_BORINGSSL_BUILD_DIR}/libcrypto.a")
    target_include_directories(p2pav PRIVATE "${_BORINGSSL_SRC_DIR}/include")
elseif(WIN32 AND EXISTS "${_BORINGSSL_BUILD_DIR}/ssl.lib")
    target_link_libraries(p2pav PRIVATE
        "${_BORINGSSL_BUILD_DIR}/ssl.lib"
        "${_BORINGSSL_BUILD_DIR}/crypto.lib")
    target_include_directories(p2pav PRIVATE "${_BORINGSSL_SRC_DIR}/include")
else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(p2pav PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(WIN32)
    target_link_libraries(p2pav PUBLIC stdc++ ws2_32)
else()
    target_link_libraries(p2pav PUBLIC stdc++ m Threads::Threads)
endif()

# --- Executables (link against shared libp2pav + media dependencies) ---
# Media-specific sources compiled directly into the executables
set(P2P_MEDIA_SOURCES
    src/p2p_publisher.c
    src/p2p_subscriber.c
    src/p2p_av_capture.c
    src/p2p_av_codec.c
    src/p2p_sdl_render.c
)

add_executable(p2p_client src/p2p_client.c ${P2P_MEDIA_SOURCES})
target_include_directories(p2p_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(p2p_client PRIVATE p2pav)

add_executable(p2p_peer src/p2p_peer.c ${P2P_MEDIA_SOURCES})
target_include_directories(p2p_peer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(p2p_peer PRIVATE p2pav)

# FFmpeg for executables
if(WIN32)
    pkg_check_modules(AVCODEC    REQUIRED libavcodec)
    pkg_check_modules(AVUTIL     REQUIRED libavutil)
    pkg_check_modules(SWSCALE    REQUIRED libswscale)
    pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
    pkg_check_modules(AVFORMAT   REQUIRED libavformat)
    pkg_check_modules(AVDEVICE   REQUIRED libavdevice)

    foreach(_tgt p2p_client p2p_peer)
        target_include_directories(${_tgt} PRIVATE
            ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS}
            ${SWSCALE_INCLUDE_DIRS} ${SWRESAMPLE_INCLUDE_DIRS}
            ${AVFORMAT_INCLUDE_DIRS} ${AVDEVICE_INCLUDE_DIRS})
        target_link_libraries(${_tgt} PRIVATE
            ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES}
            ${SWSCALE_LIBRARIES} ${SWRESAMPLE_LIBRARIES}
            ${AVFORMAT_LIBRARIES} ${AVDEVICE_LIBRARIES})
        target_link_directories(${_tgt} PRIVATE
            ${AVCODEC_LIBRARY_DIRS} ${AVUTIL_LIBRARY_DIRS}
            ${SWSCALE_LIBRARY_DIRS} ${SWRESAMPLE_LIBRARY_DIRS}
            ${AVFORMAT_LIBRARY_DIRS} ${AVDEVICE_LIBRARY_DIRS})
    endforeach()
else()
    pkg_check_modules(AVCODEC    REQUIRED libavcodec)
    pkg_check_modules(AVUTIL     REQUIRED libavutil)
    pkg_check_modules(SWSCALE    REQUIRED libswscale)
    pkg_check_modules(SWRESAMPLE REQUIRED libswresample)

    foreach(_tgt p2p_client p2p_peer)
        target_include_directories(${_tgt} PRIVATE
            ${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS}
            ${SWSCALE_INCLUDE_DIRS} ${SWRESAMPLE_INCLUDE_DIRS})
        target_link_libraries(${_tgt} PRIVATE
            ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES}
            ${SWSCALE_LIBRARIES} ${SWRESAMPLE_LIBRARIES})
        target_link_directories(${_tgt} PRIVATE
            ${AVCODEC_LIBRARY_DIRS} ${AVUTIL_LIBRARY_DIRS}
            ${SWSCALE_LIBRARY_DIRS} ${SWRESAMPLE_LIBRARY_DIRS})
    endforeach()
endif()

# SDL2 for executables
pkg_check_modules(SDL2 REQUIRED sdl2)
foreach(_tgt p2p_client p2p_peer)
    target_include_directories(${_tgt} PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(${_tgt} PRIVATE ${SDL2_LIBRARIES})
    target_link_directories(${_tgt} PRIVATE ${SDL2_LIBRARY_DIRS})
endforeach()

# ALSA (Linux only) for executables
if(NOT WIN32)
    pkg_check_modules(ALSA REQUIRED alsa)
    foreach(_tgt p2p_client p2p_peer)
        target_include_directories(${_tgt} PRIVATE ${ALSA_INCLUDE_DIRS})
        target_link_libraries(${_tgt} PRIVATE ${ALSA_LIBRARIES})
        target_link_directories(${_tgt} PRIVATE ${ALSA_LIBRARY_DIRS})
    endforeach()
endif()
