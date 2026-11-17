# =============================================================================
#  VORTEX unit tests (GoogleTest)
# =============================================================================
#  tests/core/     -> VortexCoreTests     core, math, voxel, fx  (no GPU, no window)
#  tests/physics/  -> VortexPhysicsTests  Jolt integration
#  tests/sim/      -> VortexSimTests      sim::Simulation and the server scene; links Vortex::Sim only,
#                                         so nothing of Vulkan, GLFW or ImGui is in the binary
#  tests/net/      -> VortexNetTests      the session protocol: transports, messages, server and clients
#                                         in one process; links Vortex::Net only (GPU-free, checked)
#  tests/graphics/ -> VortexGraphicsTests shader reflection / layout contracts (CPU only, no Vulkan device)
#  tests/engine/   -> VortexEngineTests   scene / engine / editor logic that needs no GPU
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
    target_link_libraries(${name} PRIVATE ${ARGN} GTest::gtest_main vortex_warnings)
    # Shader binaries and other generated assets some tests inspect.
    target_compile_definitions(${name} PRIVATE
        VORTEX_TEST_SHADER_DIR="${VORTEX_SHADER_OUTPUT_DIR}"
        VORTEX_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
    )
    add_dependencies(${name} VortexShaders)
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
set(VORTEX_SERIAL_TESTS_VortexNetTests "EnetSession.*")
vortex_add_tests(VortexNetTests net VortexNet)
vortex_assert_gpu_free(VortexNetTests)
vortex_add_tests(VortexGraphicsTests graphics VortexGraphics)
vortex_add_tests(VortexEngineTests engine VortexEngine)
