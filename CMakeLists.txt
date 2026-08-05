cmake_minimum_required(VERSION 3.25)
project(logger VERSION 0.1.0 DESCRIPTION "thin facade over spdlog" LANGUAGES CXX)

# Meant to be consumed via add_subdirectory()/CPMAddPackage() by another
# project's own CMakeLists.txt as much as built standalone - tests and
# sanitizers default off there, same reasoning as dbgen4's CMakeLists.txt.
option(BUILD_TESTING "Build the testing tree." ${PROJECT_IS_TOP_LEVEL})
include(CTest)

if(NOT CMAKE_GENERATOR)
    set(CMAKE_GENERATOR "Ninja" CACHE STRING "Force Ninja generator" FORCE)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build." FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "Profile" "Coverage")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(_DEBUG)
else()
    add_compile_definitions(NDEBUG)
endif()

# ===================================================================
# Compiler warnings - carried by an interface target, not
# add_compile_options(), so CPM dependencies below don't get -Werror'd on
# their own code.
# ===================================================================
add_library(logger_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(logger_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror -Wnarrowing)
    target_link_options(logger_warnings INTERFACE -fno-omit-frame-pointer)
endif()

# ===================================================================
# Sanitizers - directory scoped (add_compile_options), so as a subdirectory
# of someone else's project this would sanitize their targets too - default
# off there, same as BUILD_TESTING above.
#
# Mutually exclusive with ENABLE_COVERAGE below: gcov instrumentation and
# ASan both hook allocation/branch behavior, and running both at once is
# more likely to produce confusing results than useful ones - coverage wins
# if both are somehow requested.
# ===================================================================
option(ENABLE_SANITIZERS "Enable Address/Undefined/Leak sanitizers in Debug builds" ${PROJECT_IS_TOP_LEVEL})
option(ENABLE_COVERAGE "Instrument logger/logger_test with gcov and add a 'coverage' target" OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND ENABLE_SANITIZERS AND NOT ENABLE_COVERAGE)
    add_compile_options(-fsanitize=address,undefined,leak -fno-omit-frame-pointer -g3 -O1)
    add_link_options(-fsanitize=address,undefined,leak)
endif()

# ===================================================================
# Coverage - gcov instrumentation on the library and test binary, with a
# `coverage` target that runs the tests and renders an HTML report via
# gcovr. Requires BUILD_TESTING (there is nothing to run coverage on
# otherwise) and GCC (gcov is GCC's own tool; Clang's equivalent uses a
# different flag/toolchain and isn't wired up here).
#
# Deliberately *not* add_compile_options()/add_link_options() (directory
# scoped): that would also instrument logger_demo, which the `coverage`
# target never runs. logger.hpp's template member functions (trace/debug/../
# error<E>/critical<E>) get instantiated anew in every translation unit that
# calls them, demo.cpp included - an instrumented-but-never-executed
# instantiation in logger_demo would show up as "not covered" in gcovr's
# per-source-file report and drag down include/logger/logger.hpp's numbers,
# even though logger_test's own instantiations of those exact same lines are
# fully exercised. LOGGER_COVERAGE_FLAGS is applied per-target below (logger,
# logger_test, signal_terminate_helper) instead, so logger_demo stays
# uninstrumented and out of the report entirely - matching this option's own
# description ("Instrument logger/logger_test with gcov").
# ===================================================================
if(ENABLE_COVERAGE)
    if(NOT BUILD_TESTING)
        message(FATAL_ERROR "ENABLE_COVERAGE requires BUILD_TESTING (coverage is measured by running logger_test)")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "ENABLE_COVERAGE needs GCC's gcov - reconfigure with CXX=g++")
    endif()
    set(LOGGER_COVERAGE_COMPILE_FLAGS --coverage -O0 -g)
    set(LOGGER_COVERAGE_LINK_FLAGS --coverage)
endif()

# ===================================================================
# dependencies
# ===================================================================
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
include(cmake/CPM.cmake)

CPMAddPackage(
    NAME fmt
    GITHUB_REPOSITORY fmtlib/fmt
    GIT_TAG 12.2.0
    SYSTEM YES
    EXCLUDE_FROM_ALL YES
)

# SPDLOG_FMT_EXTERNAL has to agree between the spdlog built here and this
# project's own sources - a mismatch is an ODR violation that shows up as a
# link error at best. The matching compile definition is on the logger
# target below.
CPMAddPackage(
    NAME spdlog
    GITHUB_REPOSITORY gabime/spdlog
    GIT_TAG v1.17.0
    SYSTEM YES
    EXCLUDE_FROM_ALL YES
    OPTIONS
        "SPDLOG_FMT_EXTERNAL ON"
        "SPDLOG_BUILD_SHARED OFF"
        "SPDLOG_INSTALL OFF"
        "SPDLOG_BUILD_EXAMPLE OFF"
        "SPDLOG_BUILD_TESTS OFF"
)

CPMAddPackage(
    NAME nlohmann_json
    GITHUB_REPOSITORY nlohmann/json
    GIT_TAG v3.12.0
    SYSTEM YES
    EXCLUDE_FROM_ALL YES
    OPTIONS "JSON_BuildTests OFF"
)

if(BUILD_TESTING)
    CPMAddPackage(
        NAME Catch2
        GITHUB_REPOSITORY catchorg/Catch2
        GIT_TAG v3.15.3
        SYSTEM YES
        EXCLUDE_FROM_ALL YES
    )
    list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
    include(Catch)
endif()

# ===================================================================
# logger library
# ===================================================================
add_library(logger STATIC
    src/logger.cpp
    src/logger_impl.cpp
    src/logger_config.cpp
)
add_library(logger::logger ALIAS logger)

target_include_directories(logger
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# fmt is PUBLIC: logger.hpp includes <fmt/format.h>. spdlog and nlohmann_json
# stay PRIVATE - only the .cpp files (the Pimpl side) know about them, which
# is the point of the facade. stdc++exp is PUBLIC: it provides GCC's
# <stacktrace> implementation (std::stacktrace_entry::_Info::_M_populate,
# std::__stacktrace_impl::_S_current), which logger.cpp calls from an inline
# template-free function - a consumer linking liblogger.a needs it too, not
# just this build.
target_link_libraries(logger
    PUBLIC  fmt::fmt stdc++exp
    PRIVATE spdlog::spdlog nlohmann_json::nlohmann_json logger_warnings
)
target_compile_definitions(logger
    PRIVATE
        SPDLOG_FMT_EXTERNAL
        SPDLOG_COMPILED_LIB
)
if(CMAKE_BUILD_TYPE STREQUAL "Profile")
    target_compile_options(logger PRIVATE -O3 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls)
endif()
if(ENABLE_COVERAGE)
    target_compile_options(logger PRIVATE ${LOGGER_COVERAGE_COMPILE_FLAGS})
    target_link_options(logger PRIVATE ${LOGGER_COVERAGE_LINK_FLAGS})
endif()

# ===================================================================
# clang-tidy
#
# PROJECT_IS_TOP_LEVEL guarded like BUILD_TESTING/ENABLE_SANITIZERS/the demo
# below: a project pulling this in via add_subdirectory()/CPMAddPackage()
# almost always defines its own `tidy` target for its whole tree, and
# add_custom_target() aborts the configure step on a name collision (CMake
# has no way to merge or namespace custom targets the way it does real
# build targets).
# ===================================================================
find_program(RUN_CLANG_TIDY_EXECUTABLE NAMES run-clang-tidy run-clang-tidy.py)
if(RUN_CLANG_TIDY_EXECUTABLE AND PROJECT_IS_TOP_LEVEL)
    add_custom_target(tidy
        COMMAND ${RUN_CLANG_TIDY_EXECUTABLE}
            -p ${CMAKE_CURRENT_BINARY_DIR}
            -config-file=${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy
            -quiet
            -header-filter "^${CMAKE_CURRENT_SOURCE_DIR}/(include|src|demo)/.*\\.hpp$"
            -source-filter "^${CMAKE_CURRENT_SOURCE_DIR}/(src|demo)/.*\\.cpp$"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-tidy (parallel)"
        VERBATIM
    )
endif()

# ===================================================================
# config files - copied next to whichever binaries read them by default
# (demo/tests both default to a relative "config/log.debug.json" path).
# ===================================================================
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config/log.debug.json
               ${CMAKE_CURRENT_BINARY_DIR}/config/log.debug.json COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config/log.release.json
               ${CMAKE_CURRENT_BINARY_DIR}/config/log.release.json COPYONLY)

# ===================================================================
# demo program - a small, runnable illustration of Logger's main
# properties. Built whenever this is the top-level project; as a
# subdirectory of someone else's build it stays out of the way, same
# reasoning as BUILD_TESTING/ENABLE_SANITIZERS above.
# ===================================================================
if(PROJECT_IS_TOP_LEVEL)
    add_executable(logger_demo demo/demo.cpp demo/error_info.cpp)
    target_link_libraries(logger_demo PRIVATE logger::logger fmt::fmt logger_warnings)

    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND ENABLE_SANITIZERS)
        target_compile_options(logger_demo PRIVATE -fsanitize=address,undefined,leak -fno-omit-frame-pointer -g3 -O1)
        target_link_options(logger_demo PRIVATE -fsanitize=address,undefined,leak)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Profile")
        target_compile_options(logger_demo PRIVATE -O3 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls)
    endif()
    if(ENABLE_COVERAGE)
        # liblogger.a is itself instrumented (see LOGGER_COVERAGE_COMPILE_FLAGS
        # above), which makes its object files reference __gcov_init/__gcov_exit
        # - any executable linking it needs -lgcov to resolve those, even one
        # that (deliberately, see the comment above) is not itself compiled
        # with --coverage and so instantiates no instrumented template code of
        # its own.
        target_link_options(logger_demo PRIVATE ${LOGGER_COVERAGE_LINK_FLAGS})
    endif()
endif()

# ===================================================================
# tests
#
# PROJECT_IS_TOP_LEVEL guarded like tidy/coverage/the demo above:
# BUILD_TESTING is a cache variable, so a consumer that turns it ON for its
# own test tree (dbgen4, for one) would otherwise still reach this block and
# register logger_test with ctest - which then reports it as
# logger_test_NOT_BUILT because nothing here builds it as a subdirectory.
# ===================================================================
if(BUILD_TESTING AND PROJECT_IS_TOP_LEVEL)
    add_executable(logger_test test/test_logger.cpp)
    target_link_libraries(logger_test PRIVATE logger::logger Catch2::Catch2WithMain logger_warnings)
    target_include_directories(logger_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    # Absolute path to signal_terminate_helper (built below), so tests that
    # fork/exec it don't depend on the current working directory.
    target_compile_definitions(logger_test PRIVATE "SIGNAL_TERMINATE_HELPER_PATH=\"$<TARGET_FILE:signal_terminate_helper>\"")

    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND ENABLE_SANITIZERS)
        target_compile_options(logger_test PRIVATE -fsanitize=address,undefined,leak -fno-omit-frame-pointer -g3 -O1)
        target_link_options(logger_test PRIVATE -fsanitize=address,undefined,leak)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Profile")
        target_compile_options(logger_test PRIVATE -O3 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls)
    endif()
    if(ENABLE_COVERAGE)
        target_compile_options(logger_test PRIVATE ${LOGGER_COVERAGE_COMPILE_FLAGS})
        target_link_options(logger_test PRIVATE ${LOGGER_COVERAGE_LINK_FLAGS})
    endif()

    catch_discover_tests(logger_test
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        PROPERTIES LABELS "unit"
    )

    # ===================================================================
    # signal_terminate_helper - a plain, Catch2-free executable that
    # test_logger.cpp forks/execs to exercise setup_signal_handler()/
    # setup_terminate_handler(), which end in std::exit()/std::abort() and so
    # cannot run inside the Catch2 test binary itself. See the file's own
    # header comment for why. Given the same coverage instrumentation as
    # logger_test below so the library code paths it exercises still count
    # towards the coverage report.
    # ===================================================================
    add_executable(signal_terminate_helper test/helper/signal_terminate_helper.cpp)
    target_link_libraries(signal_terminate_helper PRIVATE logger::logger logger_warnings)

    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND ENABLE_SANITIZERS)
        target_compile_options(signal_terminate_helper PRIVATE -fsanitize=address,undefined,leak -fno-omit-frame-pointer -g3 -O1)
        target_link_options(signal_terminate_helper PRIVATE -fsanitize=address,undefined,leak)
    elseif(CMAKE_BUILD_TYPE STREQUAL "Profile")
        target_compile_options(signal_terminate_helper PRIVATE -O3 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls)
    endif()
    if(ENABLE_COVERAGE)
        target_compile_options(signal_terminate_helper PRIVATE ${LOGGER_COVERAGE_COMPILE_FLAGS})
        target_link_options(signal_terminate_helper PRIVATE ${LOGGER_COVERAGE_LINK_FLAGS})
        # __gcov_dump() (see the file's own comment) is only declared/linkable
        # when this binary is itself built with --coverage.
        target_compile_definitions(signal_terminate_helper PRIVATE LOGGER_TEST_COVERAGE_BUILD)
    endif()

    # ===================================================================
    # coverage - runs logger_test (discarding its own pass/fail: the point
    # here is the .gcda files it leaves behind, not a second test report),
    # then renders an HTML report with gcovr. demo/ and test/ are excluded
    # from the report - coverage is a question about the library
    # (src/, include/), not about the demo program or the test code itself.
    #
    # PROJECT_IS_TOP_LEVEL guarded like the tidy target above: BUILD_TESTING
    # is a cache variable, so a consumer that turns it ON for its own test
    # tree (dbgen4, for one) would otherwise still hit this block and collide
    # with its own `coverage` target.
    # ===================================================================
    if(ENABLE_COVERAGE AND PROJECT_IS_TOP_LEVEL)
        find_program(GCOVR_EXECUTABLE gcovr REQUIRED)
        set(COVERAGE_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/coverage)
        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E make_directory ${COVERAGE_OUTPUT_DIR}
            COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/logger.dir/src/*.gcda
            COMMAND $<TARGET_FILE:logger_test> || ${CMAKE_COMMAND} -E true
            COMMAND ${GCOVR_EXECUTABLE}
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --filter ${CMAKE_CURRENT_SOURCE_DIR}/src
                --filter ${CMAKE_CURRENT_SOURCE_DIR}/include
                --exclude-unreachable-branches
                --exclude-throw-branches
                --html --html-details
                --output ${COVERAGE_OUTPUT_DIR}/index.html
                --print-summary
                ${CMAKE_CURRENT_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            DEPENDS logger_test signal_terminate_helper
            COMMENT "Running logger_test and rendering an HTML coverage report to ${COVERAGE_OUTPUT_DIR}/index.html"
            VERBATIM
        )
    endif()
endif()
