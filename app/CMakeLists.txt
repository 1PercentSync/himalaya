add_executable(himalaya_app
    src/main.cpp
    src/application.cpp
)

target_include_directories(himalaya_app PRIVATE include)

target_link_libraries(himalaya_app PRIVATE himalaya_rhi himalaya_framework)

# Clean and copy shaders to build directory after each build
add_custom_command(TARGET himalaya_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        $<TARGET_FILE_DIR:himalaya_app>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:himalaya_app>/shaders
)
