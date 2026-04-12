add_library(xatlas STATIC
        src/xatlas.cpp
)

# Public: himalaya-style include path (e.g. #include <himalaya/xatlas/xatlas.h>)
target_include_directories(xatlas PUBLIC include)

# Private: xatlas.cpp includes "xatlas.h" via relative path
target_include_directories(xatlas PRIVATE include/himalaya/xatlas)

# Suppress warnings in third-party code
if(MSVC)
    target_compile_options(xatlas PRIVATE /W0)
else()
    target_compile_options(xatlas PRIVATE -w)
endif()
