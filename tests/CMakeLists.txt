# =============================================================================
#  VORTEX unit tests (GoogleTest)
# =============================================================================
#  tests/core/     -> VortexCoreTests     core, math, voxel, fx  (no GPU, no window)
#  tests/physics/  -> VortexPhysicsTests  Jolt integration
#  tests/sim/      -> VortexSimTests      sim::Simulation and the server scene; links Vortex::Sim only,
#                                         so nothing of Vulkan, GLFW or ImGui is in the binary
#  tests/net/      -> VortexNetTests      the session protocol: transports, messages, server and clients
#                                         in one process; links Vortex::Net only (GPU-free, checked)
#  tests/graphics/ -> VortexGraphicsTests shader reflection / layout contracts (CPU only, no Vulkan device;
#                                         VORTEX_GRAPHICS only)
#  tests/engine/   -> VortexEngineTests   scene / engine / editor logic that needs no GPU (VORTEX_GRAPHICS only)
#
#  None of the tests need a Vulkan driver, so they run on CI machines and laptops
#  without a GPU. Run them with:  ctest --test-dir <build> --output-on-failure
# =============================================================================
include(GoogleTest)

# Every test is stopped after this many seconds (review of stage 7a: a test that hangs fails instead
# of holding ctest for its 1500 s default; the slowest test takes about 30 s alone).
set(VORTEX_TEST_TIMEOUT 180)

function(vortex_add_tests name dir)
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.cpp")
    if(NOT _sources)
        return()
    endif()
    add_executable(${name} ${_sources})
    target_link_libraries(${name} PRIVATE ${ARGN} GTest::gtest_main vortex_warnings vortex_cpu_check)
    # Shader binaries and other generated assets some tests inspect.
    target_compile_definitions(${name} PRIVATE
        VORTEX_TEST_SHADER_DIR="${VORTEX_SHADER_OUTPUT_DIR}"
        VORTEX_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
    )
    if(TARGET VortexShaders)
        add_dependencies(${name} VortexShaders)
    endif()
    # VORTEX_SERIAL_TESTS_<name>: a gtest filter of tests that measure on the wall clock; they are
    # registered on their own, RUN_SERIAL, so a parallel ctest does not load the machine under them.
    if(DEFINED VORTEX_SERIAL_TESTS_${name})
        gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60 TEST_FILTER "-${VORTEX_SERIAL_TESTS_${name}}"
                             PROPERTIES TIMEOUT ${VORTEX_TEST_TIMEOUT})
        gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60 TEST_FILTER "${VORTEX_SERIAL_TESTS_${name}}"
                             PROPERTIES RUN_SERIAL TRUE LABELS wall-clock TIMEOUT ${VORTEX_TEST_TIMEOUT})
    else()
        gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT ${VORTEX_TEST_TIMEOUT})
    endif()
endfunction()

vortex_add_tests(VortexCoreTests core VortexCore)
vortex_add_tests(VortexPhysicsTests physics VortexPhysics)
vortex_add_tests(VortexSimTests sim VortexSim)
vortex_assert_gpu_free(VortexSimTests)
if(TARGET VortexSimTests)
    vortex_assert_no_network(VortexSimTests)
endif()
# The GPU-free stage-5 acceptance asserts prediction quality on the wall clock (networking.md).
set(VORTEX_SERIAL_TESTS_VortexNetTests "EnetSession.*:Relay.*")   # wall clock: ENet sessions, the UDP relay (stage 8)
vortex_add_tests(VortexNetTests net VortexNet)
vortex_assert_gpu_free(VortexNetTests)
if(VORTEX_GRAPHICS)
    vortex_add_tests(VortexGraphicsTests graphics VortexGraphics)
    vortex_add_tests(VortexEngineTests engine VortexEngine)
endif()

# The start-up CPU check refuses a CPU without the build's instructions (src/startup/CpuCheck.c): with
# VORTEX_CPU_CHECK_PRETEND_MISSING=avx2 a test binary, vortex_server and vortex_bot must exit 4 naming
# AVX2 (the game registers CpuCheck.GameRefusesAMissingFeature for its own binary).
set(_vortex_cpu_check_programs "$<TARGET_FILE:VortexCoreTests>")
foreach(_tool vortex_server vortex_bot)
    if(TARGET ${_tool})
        string(APPEND _vortex_cpu_check_programs "|$<TARGET_FILE:${_tool}>")   # '|': no list in a test argument
    endif()
endforeach()
add_test(NAME CpuCheck.RefusesAMissingFeature
         COMMAND ${CMAKE_COMMAND} "-DPROGRAMS=${_vortex_cpu_check_programs}" -P "${CMAKE_CURRENT_SOURCE_DIR}/CpuCheckRefuses.cmake")
set_tests_properties(CpuCheck.RefusesAMissingFeature PROPERTIES TIMEOUT 60)
