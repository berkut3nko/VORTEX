# The build identity (vortex::GetBuildInfo, the `Build:` log line, --version): the engine's and the
# game's commits, the build type, the compiler and the CPU flags. Included from engine/CMakeLists.txt
# after VortexCore and vortex_options exist. The header is written at configure time and refreshed
# by vortex_build_info on every build; only src/core/internal/Build.cpp includes it.
set(VORTEX_BUILD_INFO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/vortex_generated/VortexBuildInfo.h")
set(_vortex_cpu_flags "")
if(_VORTEX_JOLT_OPTIONS)
    # The instruction-set flags (-mavx2 ...), each once (Jolt lists some twice).
    set(_vortex_cpu_list ${_VORTEX_JOLT_OPTIONS})
    list(FILTER _vortex_cpu_list INCLUDE REGEX "^-m")
    list(REMOVE_DUPLICATES _vortex_cpu_list)
    list(JOIN _vortex_cpu_list " " _vortex_cpu_flags)
endif()
if(CMAKE_CONFIGURATION_TYPES)
    set(_vortex_build_type "$<CONFIG>")
else()
    set(_vortex_build_type "${CMAKE_BUILD_TYPE}")
endif()
set(_vortex_build_info_args
    "-DENGINE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DTOP_DIR=${CMAKE_SOURCE_DIR}"
    "-DTOP_NAME=${CMAKE_PROJECT_NAME}"
    "-DOUT=${VORTEX_BUILD_INFO_HEADER}"
    "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    "-DCPU_FLAGS=${_vortex_cpu_flags}"
    -P "${CMAKE_CURRENT_LIST_DIR}/GenerateBuildInfo.cmake")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vortex_generated")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DBUILD_TYPE=${CMAKE_BUILD_TYPE}" ${_vortex_build_info_args})
add_custom_target(vortex_build_info ALL
    COMMAND "${CMAKE_COMMAND}" "-DBUILD_TYPE=${_vortex_build_type}" ${_vortex_build_info_args}
    BYPRODUCTS "${VORTEX_BUILD_INFO_HEADER}"
    VERBATIM)
add_dependencies(VortexCore vortex_build_info)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/core/internal/Build.cpp" TARGET_DIRECTORY VortexCore
    PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/vortex_generated")
