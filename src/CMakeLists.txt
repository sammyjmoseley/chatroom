cmake_minimum_required(VERSION 3.8)
project(src)
#include(FindProtobuf)
#find_package(Protobuf REQUIRED)
#include_directories(${PROTOBUF_INCLUDE_DIR})

include_directories(lib/)
include_directories(/usr/local/include/)
#include_directories(/usr/bin/ld)
#include_directories(/usr/local/lib/)
#include_directories(${CMAKE_BINARY_DIR}/messages)



#set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
#set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_CXX_STANDARD 11)

set(SOURCE_FILES main.cpp)
add_executable(src ${SOURCE_FILES})

#TARGET_LINK_LIBRARIES(protobuf/)
#TARGET_LINK_LIBRARIES(src protobuf /usr/local/lib/)
