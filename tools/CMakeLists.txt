# Engine tools. Each tool is a small executable linking only what it needs.
file(GLOB _vortex_tool_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(_dir ${_vortex_tool_dirs})
    if(IS_DIRECTORY "${_dir}" AND EXISTS "${_dir}/CMakeLists.txt")
        add_subdirectory("${_dir}")
    endif()
endforeach()
