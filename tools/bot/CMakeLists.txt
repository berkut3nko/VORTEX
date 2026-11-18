# vortex_bot: one bot player in its own process (docs/design/networking.md "Stage 8": the soak).
# Links Vortex::Net only, so it builds and runs without a GPU or a window (checked at configure time).
add_executable(vortex_bot main.cpp)
target_link_libraries(vortex_bot PRIVATE VortexNet vortex_warnings)
vortex_assert_gpu_free(vortex_bot)
