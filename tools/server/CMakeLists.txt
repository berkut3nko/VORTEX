# vortex_server: the session server (docs/design/networking.md). Links Vortex::Net only, so it
# builds and runs without a GPU or a window (checked at configure time).
add_executable(vortex_server main.cpp)
target_link_libraries(vortex_server PRIVATE VortexNet vortex_warnings vortex_cpu_check)
vortex_assert_gpu_free(vortex_server)
