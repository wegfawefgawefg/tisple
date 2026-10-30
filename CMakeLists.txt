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
# the sdl window front end builds separately, so the core and the tests never need sdl
list(FILTER TISPLE_SOURCES EXCLUDE REGEX ".*/src/machine/sdl_.*\\.cpp$")
add_library(tisple_core STATIC ${TISPLE_SOURCES})
target_include_directories(tisple_core PUBLIC src third_party/chips)
tisple_warnings(tisple_core)
# lib/ is where imports look after the entry file's folder
target_compile_definitions(tisple_core PRIVATE TISPLE_LIB_DIR="${CMAKE_SOURCE_DIR}/lib")
# vendored single-file C library: no warnings-as-errors for its one compile unit
set_source_files_properties(src/emu/chips_impl.cpp PROPERTIES COMPILE_OPTIONS "-w;-O2")
# the simulator runs millions of cycles per test run (differential tests, fuzzing): optimise
# it even in debug builds
set_source_files_properties(src/emu/machine.cpp src/emu/harness.cpp PROPERTIES COMPILE_OPTIONS "-O2")
# the gap finder traces every instruction and the superoptimizer runs millions of candidate
# sequences: optimise them too
file(GLOB TISPLE_FAST_SOURCES CONFIGURE_DEPENDS src/superopt/*.cpp src/gaps/profile.cpp)
set_source_files_properties(${TISPLE_FAST_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2")

# the fuzz generator lives with the tests; `tisple gaps --fuzz` uses it too
add_executable(tisple src/main.cpp tests/fuzz_gen.cpp tests/fuzz_gen_expr.cpp
               tests/fuzz_gen_loops.cpp tests/fuzz_gen_race.cpp tests/fuzz_gen_entries.cpp
               tests/fuzz_gen_late.cpp
               tests/fuzz_gen_dead.cpp tests/fuzz_gen_motion.cpp tests/fuzz_gen_common.cpp
               tests/fuzz_gen_collapse.cpp tests/fuzz_gen_values.cpp tests/fuzz_gen_fnptr.cpp
               tests/fuzz_gen_irq.cpp tests/fuzz_gen_inline.cpp tests/fuzz_gen_size.cpp
               tests/fuzz_gen_frames.cpp
               tests/fuzz_gen_io.cpp tests/fuzz_gen_data.cpp
               tests/fuzz_gen_stretch.cpp tests/fuzz_gen_partition.cpp)
target_link_libraries(tisple PRIVATE tisple_core)
target_include_directories(tisple PRIVATE tests)
tisple_warnings(tisple)

# `tisple machine --window`: SDL3 from the system, on by default when it is found
find_package(SDL3 CONFIG QUIET)
if (NOT SDL3_FOUND)
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(SDL3 QUIET IMPORTED_TARGET sdl3)
    endif()
endif()
if (SDL3_FOUND)
    option(TISPLE_SDL "Build the SDL3 window for tisple machine --window." ON)
else()
    option(TISPLE_SDL "Build the SDL3 window for tisple machine --window." OFF)
endif()
if (TISPLE_SDL)
    file(GLOB TISPLE_SDL_SOURCES CONFIGURE_DEPENDS src/machine/sdl_*.cpp)
    add_library(tisple_window STATIC ${TISPLE_SDL_SOURCES})
    target_link_libraries(tisple_window PUBLIC tisple_core)
    if (TARGET SDL3::SDL3)
        target_link_libraries(tisple_window PRIVATE SDL3::SDL3)
    else()
        target_link_libraries(tisple_window PRIVATE PkgConfig::SDL3)
    endif()
    tisple_warnings(tisple_window)
    target_link_libraries(tisple PRIVATE tisple_window)
    target_compile_definitions(tisple PRIVATE TISPLE_HAS_SDL=1)
endif()

file(GLOB_RECURSE TISPLE_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
add_executable(tisple_tests ${TISPLE_TEST_SOURCES})
target_link_libraries(tisple_tests PRIVATE tisple_core)
target_include_directories(tisple_tests PRIVATE tests)
# reader tests read examples/ and editors/ from the source tree
target_compile_definitions(tisple_tests PRIVATE TISPLE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
tisple_warnings(tisple_tests)
