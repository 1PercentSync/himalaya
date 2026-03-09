add_library(himalaya_framework STATIC)

target_sources(himalaya_framework PRIVATE
        src/camera.cpp
        src/imgui_backend.cpp
        src/mesh.cpp
        src/render_graph.cpp
)

target_include_directories(himalaya_framework PUBLIC include)

find_package(imgui CONFIG REQUIRED)

target_link_libraries(himalaya_framework
        PUBLIC himalaya_rhi imgui::imgui
)
