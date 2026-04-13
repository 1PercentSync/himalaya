add_executable(himalaya_app
    src/main.cpp
    src/application.cpp
    src/config.cpp
    src/renderer.cpp
    src/renderer_init.cpp
    src/renderer_rasterization.cpp
    src/renderer_pt.cpp
    src/renderer_bake.cpp
    src/camera_controller.cpp
    src/debug_ui.cpp
    src/scene_loader.cpp
)

target_include_directories(himalaya_app PRIVATE include)

find_package(fastgltf CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenMP REQUIRED)

target_link_libraries(himalaya_app PRIVATE himalaya_rhi himalaya_framework himalaya_passes fastgltf::fastgltf nlohmann_json::nlohmann_json OpenMP::OpenMP_CXX)

# Clean and copy shaders to build directory after each build
add_custom_command(TARGET himalaya_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        $<TARGET_FILE_DIR:himalaya_app>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:himalaya_app>/shaders
)

# Copy OIDN runtime DLLs to build directory (all device backends + dependencies)
set(OIDN_BIN_DIR "${CMAKE_SOURCE_DIR}/third_party/oidn/bin")
file(GLOB OIDN_DLLS "${OIDN_BIN_DIR}/*.dll")
add_custom_command(TARGET himalaya_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${OIDN_DLLS}
        $<TARGET_FILE_DIR:himalaya_app>
)
