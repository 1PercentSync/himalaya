add_library(himalaya_framework STATIC)

target_sources(himalaya_framework PRIVATE
        src/cache.cpp
        src/cached_shader_compiler.cpp
        src/camera.cpp
        src/color_utils.cpp
        src/denoiser.cpp
        src/ktx2.cpp
        src/culling.cpp
        src/ibl.cpp
        src/ibl_compute.cpp
        src/cubemap_filter.cpp
        src/texture_compress.cpp
        src/imgui_backend.cpp
        src/material_system.cpp
        src/mesh.cpp
        src/scene_as_builder.cpp
        src/emissive_light_builder.cpp
        src/render_graph.cpp
        src/shadow.cpp
        src/stb_impl.cpp
        src/texture.cpp
        src/lightmap_uv.cpp
        src/lightmap_uv_generator.cpp
        src/bake_denoiser.cpp
        src/probe_placement.cpp
)

target_include_directories(himalaya_framework PUBLIC include)

find_package(imgui CONFIG REQUIRED)
find_package(mikktspace CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(xxHash CONFIG REQUIRED)

target_include_directories(himalaya_framework PRIVATE ${Stb_INCLUDE_DIR})

target_link_libraries(himalaya_framework
        PUBLIC himalaya_rhi imgui::imgui
        PRIVATE mikktspace::mikktspace nlohmann_json::nlohmann_json xxHash::xxhash bc7enc xatlas oidn
)
