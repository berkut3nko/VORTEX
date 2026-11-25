# vortex_land: bakes the world map from the drawn map (docs/design/land.md). Links Vortex::Core only.
add_executable(vortex_land main.cpp)
target_link_libraries(vortex_land PRIVATE VortexCore vortex_warnings vortex_cpu_check)
vortex_assert_gpu_free(vortex_land)
