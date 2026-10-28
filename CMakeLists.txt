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
    message(STATUS "[EduRoute] DEBUG mode: ASan + UBSan enabled")
    add_compile_options(-O0 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
else()
    message(STATUS "[EduRoute] RELEASE mode: -O3 + march=native")
    add_compile_options(-O3 -march=native -DNDEBUG)
endif()

option(ENABLE_CLANG_TIDY "Run clang-tidy automatically during compilation" OFF)

find_program(CLANG_FORMAT_EXE NAMES "clang-format")
find_program(CLANG_TIDY_EXE NAMES "clang-tidy")

file(GLOB_RECURSE ALL_CXX_SOURCE_FILES
        CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/sim/*.cpp"
)

if(CLANG_FORMAT_EXE)
    add_custom_target(format
            COMMAND ${CLANG_FORMAT_EXE} -i -style=file${ALL_CXX_SOURCE_FILES}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Auto-formatting all C/C++ files with clang-format..."
            VERBATIM
    )
endif()

if(CLANG_TIDY_EXE)
    if(ENABLE_CLANG_TIDY)
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-p=${CMAKE_BINARY_DIR}")
    endif()

    file(GLOB_RECURSE ALL_IMPL_FILES
            CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/sim/*.cpp"
    )
    add_custom_target(tidy
            COMMAND ${CLANG_TIDY_EXE} -p=${CMAKE_BINARY_DIR}${ALL_IMPL_FILES}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Running clang-tidy static analysis..."
            VERBATIM
    )
endif()


file(GLOB_RECURSE DIV1_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/sync_engine/*.cpp")
file(GLOB_RECURSE DIV2_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/anticheat/*.cpp")
file(GLOB_RECURSE DIV3_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/adaptive_rank/*.cpp")

add_library(eduroute_engine STATIC
        ${DIV1_SOURCES}
        ${DIV2_SOURCES}
        ${DIV3_SOURCES}
        include/anticheat/kmer_winnow.hpp
        include/core/dsu.hpp
)

target_include_directories(eduroute_engine PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_library(eduroute SHARED
        src/eduroute_c_api.cpp
        include/anticheat/kmer_winnow.hpp
        include/core/dsu.hpp
)

target_link_libraries(eduroute PRIVATE eduroute_engine)

set_target_properties(eduroute PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dashboard"
)

add_executable(stress_sim sim/stress_sim.cpp
        include/anticheat/kmer_winnow.hpp
        include/core/dsu.hpp)
target_link_libraries(stress_sim PRIVATE eduroute_engine)

add_executable(test_adts sim/test_adts.cpp
        include/anticheat/kmer_winnow.hpp
        include/core/dsu.hpp)
target_link_libraries(test_adts PRIVATE eduroute_engine)