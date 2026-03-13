add_library(himalaya_passes STATIC)

target_sources(himalaya_passes PRIVATE
        src/depth_prepass.cpp
        src/forward_pass.cpp
        src/skybox_pass.cpp
        src/tonemapping_pass.cpp
)

target_include_directories(himalaya_passes PUBLIC include)

target_link_libraries(himalaya_passes PUBLIC himalaya_framework)
