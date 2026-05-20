add_library(tinyply STATIC
    src/tinyply.cpp
)

# Public: himalaya-style include path (e.g. #include <himalaya/tinyply/tinyply.h>)
target_include_directories(tinyply PUBLIC include)

# Private: tinyply.cpp includes "tinyply.h" relative to the header directory
target_include_directories(tinyply PRIVATE include/himalaya/tinyply)

# Suppress warnings in third-party code
if(MSVC)
    target_compile_options(tinyply PRIVATE /W0)
else()
    target_compile_options(tinyply PRIVATE -w)
endif()
