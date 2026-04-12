add_library(oidn SHARED IMPORTED GLOBAL)
set_target_properties(oidn PROPERTIES
        IMPORTED_IMPLIB "${CMAKE_CURRENT_SOURCE_DIR}/lib/OpenImageDenoise.lib"
        IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/bin/OpenImageDenoise.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
