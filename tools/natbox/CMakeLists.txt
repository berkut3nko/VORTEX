# vortex_natbox (rendezvous.md "Tests", gate G20): an emulated NAT for one program, and a fake STUN server. Linux only.
if(NOT WIN32)
    add_executable(vortex_natbox main.cpp)
    target_link_libraries(vortex_natbox PRIVATE VortexNet vortex_warnings vortex_cpu_check)
    vortex_assert_gpu_free(vortex_natbox)
endif()
