cmake_minimum_required(VERSION 3.10)

find_package(Threads REQUIRED)
include(FindPkgConfig)

# --- Signaling protocol test ---
add_executable(test_signaling test_signaling.c)
target_link_libraries(test_signaling PRIVATE p2pav)

# --- ICE connectivity test ---
add_executable(test_ice_connectivity test_ice_connectivity.c)
target_link_libraries(test_ice_connectivity PRIVATE p2pav)

# --- libp2pav E2E test (links shared library only, no FFmpeg/SDL) ---
add_executable(test_p2pav_e2e test_p2pav_e2e.c)
target_link_libraries(test_p2pav_e2e PRIVATE p2pav)

# --- libp2pav stress / robustness / stability test ---
add_executable(test_p2pav_stress test_p2pav_stress.c)
target_link_libraries(test_p2pav_stress PRIVATE p2pav)

# --- p2p_publisher / p2p_subscriber middleware + codec test ---
add_executable(test_p2p_middleware
    test_p2p_middleware.c
    ${PROJECT_SOURCE_DIR}/p2p/src/p2p_publisher.c
    ${PROJECT_SOURCE_DIR}/p2p/src/p2p_subscriber.c
    ${PROJECT_SOURCE_DIR}/p2p/src/p2p_av_codec.c
)
target_include_directories(test_p2p_middleware PRIVATE
    ${PROJECT_SOURCE_DIR}/p2p/include)
target_link_libraries(test_p2p_middleware PRIVATE p2pav)

pkg_check_modules(MW_AVCODEC    REQUIRED libavcodec)
pkg_check_modules(MW_AVUTIL     REQUIRED libavutil)
pkg_check_modules(MW_SWSCALE    REQUIRED libswscale)
pkg_check_modules(MW_SWRESAMPLE REQUIRED libswresample)

target_include_directories(test_p2p_middleware PRIVATE
    ${MW_AVCODEC_INCLUDE_DIRS} ${MW_AVUTIL_INCLUDE_DIRS}
    ${MW_SWSCALE_INCLUDE_DIRS} ${MW_SWRESAMPLE_INCLUDE_DIRS})
target_link_libraries(test_p2p_middleware PRIVATE
    ${MW_AVCODEC_LIBRARIES} ${MW_AVUTIL_LIBRARIES}
    ${MW_SWSCALE_LIBRARIES} ${MW_SWRESAMPLE_LIBRARIES})
target_link_directories(test_p2p_middleware PRIVATE
    ${MW_AVCODEC_LIBRARY_DIRS} ${MW_AVUTIL_LIBRARY_DIRS}
    ${MW_SWSCALE_LIBRARY_DIRS} ${MW_SWRESAMPLE_LIBRARY_DIRS})
