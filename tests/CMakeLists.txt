cmake_minimum_required(VERSION 3.10)

find_package(Threads REQUIRED)

# --- Signaling protocol test (no xquic dependency needed) ---
add_executable(test_signaling test_signaling.c
    ${CMAKE_SOURCE_DIR}/p2p/src/p2p_signaling.c)
target_include_directories(test_signaling PRIVATE ${CMAKE_SOURCE_DIR}/p2p/include)
target_link_libraries(test_signaling PRIVATE Threads::Threads)

# --- ICE connectivity test ---
add_executable(test_ice_connectivity test_ice_connectivity.c)
target_link_libraries(test_ice_connectivity PRIVATE p2p_av)
