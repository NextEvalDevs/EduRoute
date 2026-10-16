cmake_minimum_required(VERSION 3.20)
project(EduRoute VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
        -Wno-unused-parameter
)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "[EduRoute] Building in DEBUG mode with ASan + UBSan enabled")
    add_compile_options(-O0 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
else()
    message(STATUS "[EduRoute] Building in RELEASE mode (-O3 + march=native)")
    add_compile_options(-O3 -march=native -DNDEBUG)
endif()

file(GLOB_RECURSE DIV1_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/sync_engine/*.cpp")
file(GLOB_RECURSE DIV2_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/anticheat/*.cpp")
file(GLOB_RECURSE DIV3_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/adaptive_rank/*.cpp")


add_library(eduroute_engine STATIC
        ${DIV1_SOURCES}
        ${DIV2_SOURCES}
        ${DIV3_SOURCES}
)

target_include_directories(eduroute_engine PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

#add_library(eduroute SHARED
#        src/eduroute_c_api.cpp
#)

#target_link_libraries(eduroute PRIVATE eduroute_engine)

#
#set_target_properties(eduroute PROPERTIES
#        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dashboard"
#)
