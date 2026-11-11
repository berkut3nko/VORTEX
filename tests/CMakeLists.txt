# =============================================================================
#  VORTEX unit tests (GoogleTest)
# =============================================================================
#  tests/core/     -> VortexCoreTests     core, math, voxel, fx  (no GPU, no window)
#  tests/physics/  -> VortexPhysicsTests  Jolt integration
#  tests/sim/      -> VortexSimTests      sim::Simulation and the server scene; links Vortex::Sim only,
#                                         so nothing of Vulkan, GLFW or ImGui is in the binary
#  tests/graphics/ -> VortexGraphicsTests shader reflection / layout contracts (CPU only, no Vulkan device)
#  tests/engine/   -> VortexEngineTests   scene / engine / editor logic that needs no GPU
#
#  None of the tests need a Vulkan driver, so they run on CI machines and laptops
#  without a GPU. Run them with:  ctest --test-dir <build> --output-on-failure
# =============================================================================
include(GoogleTest)

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
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60)
endfunction()

vortex_add_tests(VortexCoreTests core VortexCore)
vortex_add_tests(VortexPhysicsTests physics VortexPhysics)
vortex_add_tests(VortexSimTests sim VortexSim)
vortex_assert_gpu_free(VortexSimTests)
vortex_add_tests(VortexGraphicsTests graphics VortexGraphics)
vortex_add_tests(VortexEngineTests engine VortexEngine)
