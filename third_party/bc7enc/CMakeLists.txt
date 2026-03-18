add_library(bc7enc STATIC
        src/rgbcx.cpp
        src/bc7e.ispc
)

# Public: himalaya-style include path (e.g. #include <himalaya/bc7enc/rgbcx.h>)
target_include_directories(bc7enc PUBLIC include)

# Public: ISPC auto-generated headers (e.g. bc7e_ispc.h) land in the binary dir
target_include_directories(bc7enc PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

# Private: rgbcx source includes headers relative to include/himalaya/bc7enc/
target_include_directories(bc7enc PRIVATE include/himalaya/bc7enc)

# ISPC: target multiple SIMD widths; ISPC runtime picks the best at call time
set_source_files_properties(src/bc7e.ispc PROPERTIES
        COMPILE_OPTIONS "--opt=fast-math;--opt=disable-assertions"
)
set_property(TARGET bc7enc PROPERTY ISPC_INSTRUCTION_SETS sse2-i32x4;sse4-i32x4;avx2-i32x8;avx512skx-i32x16)

# Suppress warnings in third-party code (CXX only; ISPC has its own flags)
if(MSVC)
    target_compile_options(bc7enc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W0>)
else()
    target_compile_options(bc7enc PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-w>)
endif()
