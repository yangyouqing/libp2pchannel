cmake_minimum_required(VERSION 3.10)
project(p2pav_examples C)

add_executable(p2pav_pub_example p2pav_publisher_example.c)
target_link_libraries(p2pav_pub_example PRIVATE p2pav)

add_executable(p2pav_sub_example p2pav_subscriber_example.c)
target_link_libraries(p2pav_sub_example PRIVATE p2pav)
