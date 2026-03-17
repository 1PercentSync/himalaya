add_library(bc7enc STATIC
        src/bc7enc.cpp
        src/rgbcx.cpp
)

# Public: himalaya-style include path (e.g. #include <himalaya/bc7enc/bc7enc.h>)
target_include_directories(bc7enc PUBLIC include)

# Private: source files use #include "bc7enc.h" (relative to include/himalaya/bc7enc/)
target_include_directories(bc7enc PRIVATE include/himalaya/bc7enc)

# Suppress warnings in third-party code
if(MSVC)
    target_compile_options(bc7enc PRIVATE /W0)
else()
    target_compile_options(bc7enc PRIVATE -w)
endif()
