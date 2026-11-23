# vortex_https (rendezvous.md "The engine side"): net::HttpsClient over the system's libcurl (a recorded exception
# to pinned dependencies: it is the system's TLS). Linked only by vortex_server and the game, never by Vortex::Net.
find_package(CURL REQUIRED)
add_library(vortex_https STATIC)
target_sources(vortex_https
    PRIVATE internal/CurlHttps.cpp
    PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" FILES Https.cppm)
target_link_libraries(vortex_https PUBLIC VortexNet vortex_options PRIVATE CURL::libcurl vortex_warnings)
vortex_assert_gpu_free(vortex_https)
