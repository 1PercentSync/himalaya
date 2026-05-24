add_library(himalaya_passes STATIC)

target_sources(himalaya_passes PRIVATE
        src/gs_projection_pass.cpp
        src/gs_tile_binning_pass.cpp
        src/gs_tile_render_pass.cpp
        src/present_pass.cpp
        src/reference_view_pass.cpp
)

target_include_directories(himalaya_passes PUBLIC include)

target_link_libraries(himalaya_passes PUBLIC himalaya_framework)
