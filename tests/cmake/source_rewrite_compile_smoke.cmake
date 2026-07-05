foreach(required_var IN ITEMS REWRITE_EXE SOURCE_FILE OUTPUT_DIR INCLUDE_DIR CXX_COMPILER)
    if (NOT DEFINED ${required_var})
        message(FATAL_ERROR "missing required variable ${required_var}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(REWRITTEN_SOURCE "${OUTPUT_DIR}/source_rewrite_compile_output.cpp")
set(REWRITTEN_EXE "${OUTPUT_DIR}/source_rewrite_compile_output")

execute_process(
    COMMAND "${REWRITE_EXE}" "--output=${REWRITTEN_SOURCE}" "${SOURCE_FILE}"
    RESULT_VARIABLE rewrite_result
    OUTPUT_VARIABLE rewrite_stdout
    ERROR_VARIABLE rewrite_stderr
)
if (NOT rewrite_result EQUAL 0)
    message(FATAL_ERROR "lincheck_source_rewrite failed (${rewrite_result})\nstdout:\n${rewrite_stdout}\nstderr:\n${rewrite_stderr}")
endif()

execute_process(
    COMMAND "${CXX_COMPILER}" -std=c++20 -I "${INCLUDE_DIR}" "${REWRITTEN_SOURCE}" -pthread -o "${REWRITTEN_EXE}"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr
)
if (NOT compile_result EQUAL 0)
    message(FATAL_ERROR "rewritten source failed to compile (${compile_result})\nstdout:\n${compile_stdout}\nstderr:\n${compile_stderr}")
endif()

execute_process(
    COMMAND "${REWRITTEN_EXE}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr
)
if (NOT run_result EQUAL 0)
    message(FATAL_ERROR "rewritten source executable failed (${run_result})\nstdout:\n${run_stdout}\nstderr:\n${run_stderr}")
endif()
