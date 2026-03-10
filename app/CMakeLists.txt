add_executable(himalaya_app
    src/main.cpp
    src/application.cpp
    src/camera_controller.cpp
    src/debug_ui.cpp
    src/scene_loader.cpp
)

target_include_directories(himalaya_app PRIVATE include)

find_package(fastgltf CONFIG REQUIRED)

target_link_libraries(himalaya_app PRIVATE himalaya_rhi himalaya_framework fastgltf::fastgltf)

# Clean and copy shaders to build directory after each build
add_custom_command(TARGET himalaya_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        $<TARGET_FILE_DIR:himalaya_app>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:himalaya_app>/shaders
)

# Clean and copy assets to build directory after each build
add_custom_command(TARGET himalaya_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        $<TARGET_FILE_DIR:himalaya_app>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:himalaya_app>/assets
)
