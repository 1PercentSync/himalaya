add_library(himalaya_framework STATIC)

target_sources(himalaya_framework PRIVATE
        src/cache.cpp
        src/camera.cpp
        src/ktx2.cpp
        src/culling.cpp
        src/ibl.cpp
        src/imgui_backend.cpp
        src/material_system.cpp
        src/mesh.cpp
        src/render_graph.cpp
        src/stb_impl.cpp
        src/texture.cpp
)

target_include_directories(himalaya_framework PUBLIC include)

find_package(imgui CONFIG REQUIRED)
find_package(mikktspace CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(xxHash CONFIG REQUIRED)

target_include_directories(himalaya_framework PRIVATE ${Stb_INCLUDE_DIR})

target_link_libraries(himalaya_framework
        PUBLIC himalaya_rhi imgui::imgui
        PRIVATE mikktspace::mikktspace xxHash::xxhash
)
