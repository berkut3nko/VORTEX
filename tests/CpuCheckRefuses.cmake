# CpuCheck.RefusesAMissingFeature (tests/CMakeLists.txt): each of PROGRAMS ('|'-separated), run with
# VORTEX_CPU_CHECK_PRETEND_MISSING=avx2, must exit 4 and name AVX2 on stderr before doing anything else.
string(REPLACE "|" ";" _programs "${PROGRAMS}")
foreach(_program ${_programs})
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env VORTEX_CPU_CHECK_PRETEND_MISSING=avx2 "${_program}" --version
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 30)
    if(NOT _rc EQUAL 4)
        message(FATAL_ERROR "${_program}: exit ${_rc}, expected 4 (the CPU check)\nstdout: ${_out}\nstderr: ${_err}")
    endif()
    if(NOT _err MATCHES "this CPU lacks: AVX2")
        message(FATAL_ERROR "${_program}: stderr does not name AVX2: ${_err}")
    endif()
    message(STATUS "${_program}: exit 4, ${_err}")
endforeach()
