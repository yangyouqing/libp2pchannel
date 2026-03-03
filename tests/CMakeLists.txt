cmake_minimum_required(VERSION 3.10)

find_package(Threads REQUIRED)

# --- Signaling protocol test ---
add_executable(test_signaling test_signaling.c)
target_link_libraries(test_signaling PRIVATE p2p_av)

# --- ICE connectivity test ---
add_executable(test_ice_connectivity test_ice_connectivity.c)
target_link_libraries(test_ice_connectivity PRIVATE p2p_av)
