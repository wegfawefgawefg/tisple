cmake_minimum_required(VERSION 3.20)

project(tisple VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TISPLE_WARN_AS_ERROR "Treat warnings as errors." ON)
option(TISPLE_SANITIZERS "Build with AddressSanitizer and UBSan." OFF)

# fast rebuilds: ccache and lld when present
find_program(TISPLE_CCACHE ccache)
if (TISPLE_CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${TISPLE_CCACHE}")
endif()
find_program(TISPLE_LLD ld.lld)
if (TISPLE_LLD AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_link_options(-fuse-ld=lld)
endif()

function(tisple_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Wconversion
                                             -Wno-sign-conversion)
    if (TISPLE_WARN_AS_ERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
    if (TISPLE_SANITIZERS)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined
                                                 -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# everything but main.cpp goes in one library shared by the tool and the tests
file(GLOB_RECURSE TISPLE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER TISPLE_SOURCES EXCLUDE REGEX ".*/src/main\\.cpp$")
add_library(tisple_core STATIC ${TISPLE_SOURCES})
target_include_directories(tisple_core PUBLIC src third_party/chips)
tisple_warnings(tisple_core)
# vendored single-file C library: no warnings-as-errors for its one compile unit
set_source_files_properties(src/emu/chips_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

add_executable(tisple src/main.cpp)
target_link_libraries(tisple PRIVATE tisple_core)
tisple_warnings(tisple)

file(GLOB_RECURSE TISPLE_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
add_executable(tisple_tests ${TISPLE_TEST_SOURCES})
target_link_libraries(tisple_tests PRIVATE tisple_core)
target_include_directories(tisple_tests PRIVATE tests)
# reader tests read examples/ and editors/ from the source tree
target_compile_definitions(tisple_tests PRIVATE TISPLE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
tisple_warnings(tisple_tests)
