add_library(himalaya_rhi STATIC)

target_sources(himalaya_rhi PRIVATE src/context.cpp src/swapchain.cpp src/shader.cpp src/pipeline.cpp src/commands.cpp src/resources.cpp src/descriptors.cpp src/vma_impl.cpp)

target_include_directories(himalaya_rhi PUBLIC include)

find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(unofficial-shaderc CONFIG REQUIRED)

target_link_libraries(himalaya_rhi PUBLIC
        Vulkan::Vulkan
        glfw
        spdlog::spdlog
        GPUOpen::VulkanMemoryAllocator
        unofficial::shaderc::shaderc
)