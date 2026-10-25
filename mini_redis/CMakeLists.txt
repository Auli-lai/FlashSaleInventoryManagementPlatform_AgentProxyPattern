cmake_minimum_required(VERSION 3.10)
project(mini_redis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -pthread")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

set(SOURCE_FILES
    src/main.cpp
    src/server.cpp
    src/epoll.cpp
    src/log.cpp
    src/timer.cpp
    src/resp_parser.cpp
    src/db.cpp
    src/command.cpp
)

set(HEADER_FILES
    include/server.h
    include/epoll.h
    include/log.h
    include/queue.h
    include/timer.h
    include/resp_parser.h
    include/db.h
    include/command.h
)

add_executable(mini_redis ${SOURCE_FILES} ${HEADER_FILES})

if(UNIX)
    target_link_libraries(mini_redis pthread)
endif()
