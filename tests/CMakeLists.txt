cmake_minimum_required(VERSION 3.20)
project(webm_player_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(CPP_ROOT "${PROJECT_ROOT}/cpp")

# The shared C++ compiles on the host via MEDIA_PLATFORM_HOST (see Platform.h).
# No -DTARGET_OS_IOS fiction required.

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# -DSANITIZER=address|thread|undefined|none
if(NOT DEFINED SANITIZER)
    set(SANITIZER "address" CACHE STRING "address, thread, undefined, none")
endif()

if(SANITIZER STREQUAL "address")
    message(STATUS "Building with AddressSanitizer + LeakSanitizer")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address)
elseif(SANITIZER STREQUAL "thread")
    message(STATUS "Building with ThreadSanitizer")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=thread)
elseif(SANITIZER STREQUAL "undefined")
    message(STATUS "Building with UndefinedBehaviorSanitizer")
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=undefined)
elseif(SANITIZER STREQUAL "none")
    message(STATUS "Building without sanitizer")
    add_compile_options(-g -O0)
else()
    message(FATAL_ERROR "Unknown SANITIZER=${SANITIZER}")
endif()

include_directories(
    ${CPP_ROOT}
    ${CPP_ROOT}/common
)

add_executable(test_stream_buffer
    test_stream_buffer.cpp
    ${CPP_ROOT}/common/WebMStreamBuffer.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(test_stream_buffer PRIVATE Threads::Threads)
