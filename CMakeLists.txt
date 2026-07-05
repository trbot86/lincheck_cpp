cmake_minimum_required(VERSION 3.20)

project(cpp_lincheck_mvp LANGUAGES CXX)

option(LINCHECK_BUILD_MULTIVERSE_TESTS "Build Multiverse-backed smoke tests when the editable checkout is available" ON)
option(LINCHECK_BUILD_CLANG_TOOLS "Build optional Clang/LLVM tooling prototypes when LibTooling is available" ON)
set(LINCHECK_MULTIVERSE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/upstream/mvcc_tm"
    CACHE PATH "Path to an mvcc_tm checkout patched with Lincheck hook call sites"
)

add_library(lincheck INTERFACE)
target_include_directories(lincheck INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(lincheck INTERFACE cxx_std_20)

add_executable(lincheck_mvp_tests
    tests/lincheck_mvp_tests.cpp
)
target_link_libraries(lincheck_mvp_tests PRIVATE lincheck)
target_compile_options(lincheck_mvp_tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

set(LINCHECK_HAS_MULTIVERSE_TESTS OFF)
if (LINCHECK_BUILD_MULTIVERSE_TESTS AND EXISTS "${LINCHECK_MULTIVERSE_ROOT}/multiverse/multiverse.hpp")
    add_executable(multiverse_smoke_tests
        tests/multiverse_smoke_tests.cpp
    )
    target_link_libraries(multiverse_smoke_tests PRIVATE lincheck)
    target_include_directories(multiverse_smoke_tests SYSTEM PRIVATE
        ${LINCHECK_MULTIVERSE_ROOT}
        ${LINCHECK_MULTIVERSE_ROOT}/setbench/common
    )
    target_compile_definitions(multiverse_smoke_tests PRIVATE
        MAX_THREADS_POW2=64
        CPU_FREQ_GHZ=3.0
    )
    target_compile_options(multiverse_smoke_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    add_executable(multiverse_tiny_set_example
        examples/multiverse_tiny_set_example.cpp
    )
    target_link_libraries(multiverse_tiny_set_example PRIVATE lincheck)
    target_include_directories(multiverse_tiny_set_example SYSTEM PRIVATE
        ${LINCHECK_MULTIVERSE_ROOT}
        ${LINCHECK_MULTIVERSE_ROOT}/setbench/common
    )
    target_compile_definitions(multiverse_tiny_set_example PRIVATE
        MAX_THREADS_POW2=64
        CPU_FREQ_GHZ=3.0
    )
    target_compile_options(multiverse_tiny_set_example PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    add_executable(multiverse_bank_example
        examples/multiverse_bank_example.cpp
    )
    target_link_libraries(multiverse_bank_example PRIVATE lincheck)
    target_include_directories(multiverse_bank_example SYSTEM PRIVATE
        ${LINCHECK_MULTIVERSE_ROOT}
        ${LINCHECK_MULTIVERSE_ROOT}/setbench/common
    )
    target_compile_definitions(multiverse_bank_example PRIVATE
        MAX_THREADS_POW2=64
        CPU_FREQ_GHZ=3.0
    )
    target_compile_options(multiverse_bank_example PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
    set(LINCHECK_HAS_MULTIVERSE_TESTS ON)
elseif (LINCHECK_BUILD_MULTIVERSE_TESTS)
    message(STATUS "Skipping multiverse_smoke_tests: ${LINCHECK_MULTIVERSE_ROOT}/multiverse/multiverse.hpp was not found")
endif()

add_executable(modular_header_smoke
    tests/modular_header_smoke.cpp
)
target_link_libraries(modular_header_smoke PRIVATE lincheck)
target_compile_options(modular_header_smoke PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(lincheck_source_audit
    tools/lincheck_source_audit.cpp
)
target_link_libraries(lincheck_source_audit PRIVATE lincheck)
target_compile_options(lincheck_source_audit PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(lincheck_source_rewrite
    tools/lincheck_source_rewrite.cpp
)
target_link_libraries(lincheck_source_rewrite PRIVATE lincheck)
target_compile_options(lincheck_source_rewrite PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

set(LINCHECK_HAS_CLANG_TOOLS OFF)
if (LINCHECK_BUILD_CLANG_TOOLS)
    find_program(LINCHECK_LLVM_CONFIG_EXE llvm-config)
    if (LINCHECK_LLVM_CONFIG_EXE)
        execute_process(
            COMMAND ${LINCHECK_LLVM_CONFIG_EXE} --cmakedir
            OUTPUT_VARIABLE LINCHECK_LLVM_CMAKE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    endif()
    if (LINCHECK_LLVM_CMAKE_DIR)
        get_filename_component(LINCHECK_LLVM_CMAKE_ROOT "${LINCHECK_LLVM_CMAKE_DIR}/.." ABSOLUTE)
    endif()

    find_package(LLVM CONFIG QUIET HINTS "${LINCHECK_LLVM_CMAKE_DIR}")
    find_package(Clang CONFIG QUIET HINTS "${LINCHECK_LLVM_CMAKE_ROOT}/clang")

    if (LLVM_FOUND AND Clang_FOUND AND TARGET clangTooling)
        add_executable(lincheck_clang_source_audit
            tools/lincheck_clang_source_audit.cpp
        )
        target_include_directories(lincheck_clang_source_audit SYSTEM PRIVATE
            ${LLVM_INCLUDE_DIRS}
            ${CLANG_INCLUDE_DIRS}
        )
        target_compile_definitions(lincheck_clang_source_audit PRIVATE
            ${LLVM_DEFINITIONS}
        )
        target_compile_features(lincheck_clang_source_audit PRIVATE cxx_std_20)
        target_link_libraries(lincheck_clang_source_audit PRIVATE
            lincheck
            clangTooling
        )
        target_compile_options(lincheck_clang_source_audit PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
        set(LINCHECK_HAS_CLANG_TOOLS ON)
    else()
        message(STATUS "Skipping lincheck_clang_source_audit: Clang LibTooling CMake targets were not found")
    endif()
endif()

enable_testing()
add_test(NAME lincheck_mvp_tests COMMAND lincheck_mvp_tests)
if (LINCHECK_HAS_MULTIVERSE_TESTS)
    add_test(NAME multiverse_smoke_tests COMMAND multiverse_smoke_tests)
    add_test(NAME multiverse_tiny_set_example COMMAND multiverse_tiny_set_example)
    add_test(NAME multiverse_bank_example COMMAND multiverse_bank_example)
endif()
add_test(NAME modular_header_smoke COMMAND modular_header_smoke)
add_test(NAME lincheck_source_audit_detects_raw_sync
    COMMAND lincheck_source_audit ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/source_audit_bad.cpp
)
set_tests_properties(lincheck_source_audit_detects_raw_sync PROPERTIES WILL_FAIL TRUE)
add_test(NAME lincheck_source_audit_accepts_suppressed_raw_sync
    COMMAND lincheck_source_audit ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/source_audit_suppressed.cpp
)
add_test(NAME lincheck_source_rewrite_check_detects_changes
    COMMAND lincheck_source_rewrite --check ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/source_audit_bad.cpp
)
set_tests_properties(lincheck_source_rewrite_check_detects_changes PROPERTIES WILL_FAIL TRUE)
add_test(NAME lincheck_source_rewrite_directory_check_detects_changes
    COMMAND lincheck_source_rewrite --check ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures
)
set_tests_properties(lincheck_source_rewrite_directory_check_detects_changes PROPERTIES WILL_FAIL TRUE)
add_test(NAME lincheck_source_rewrite_rejects_directory_stdout
    COMMAND lincheck_source_rewrite ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures
)
set_tests_properties(lincheck_source_rewrite_rejects_directory_stdout PROPERTIES WILL_FAIL TRUE)
add_test(NAME lincheck_source_rewrite_runs_on_suppressed_file
    COMMAND lincheck_source_rewrite --check ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/source_audit_suppressed.cpp
)
add_test(NAME lincheck_source_rewrite_output_compiles
    COMMAND ${CMAKE_COMMAND}
        -DREWRITE_EXE=$<TARGET_FILE:lincheck_source_rewrite>
        -DSOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/source_rewrite_compile_input.cpp
        -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/source_rewrite_compile_smoke
        -DINCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/include
        -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/source_rewrite_compile_smoke.cmake
)
if (LINCHECK_HAS_CLANG_TOOLS)
    add_test(NAME lincheck_clang_source_audit_detects_raw_sync
        COMMAND lincheck_clang_source_audit
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/source_audit_bad.cpp
            --
            -std=c++20
    )
    set_tests_properties(lincheck_clang_source_audit_detects_raw_sync PROPERTIES WILL_FAIL TRUE)
    add_test(NAME lincheck_clang_source_audit_accepts_lincheck_wrappers
        COMMAND lincheck_clang_source_audit
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/clang_source_audit_clean.cpp
            --
            -std=c++20
            -I${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    add_test(NAME lincheck_clang_source_audit_accepts_suppressed_raw_sync
        COMMAND lincheck_clang_source_audit
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/source_audit_suppressed.cpp
            --
            -std=c++20
    )
    add_test(NAME lincheck_clang_source_audit_honors_path_exclude
        COMMAND lincheck_clang_source_audit
            --exclude=source_audit_bad.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/source_audit_bad.cpp
            --
            -std=c++20
    )
endif()
