cmake_minimum_required(VERSION 3.12)
project(BitTorrentProto)

FILE(GLOB_RECURSE BIT_TORRENT_SRC "./src/*.cpp")

add_library(BitTorrentProto ${BIT_TORRENT_SRC})
target_link_libraries(BitTorrentProto PUBLIC Core crypto)
target_include_directories(BitTorrentProto PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
