add_executable(himalaya_app main.cpp)

target_link_libraries(himalaya_app PRIVATE himalaya_rhi)

# Clean and copy shaders to build directory after each build
add_custom_command(TARGET himalaya_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        $<TARGET_FILE_DIR:himalaya_app>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:himalaya_app>/shaders
)