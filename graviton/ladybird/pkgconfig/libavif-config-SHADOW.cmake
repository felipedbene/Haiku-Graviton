# DeBeOS/Haiku corrected shadow of the system libavif config. The system config
# computes _IMPORT_PREFIX=/boot/system and sets include to /boot/system/include,
# which does not exist on Haiku (headers live under develop/headers). CMake treats
# a non-existent INTERFACE_INCLUDE_DIRECTORIES on an imported target as fatal, so
# we define the target with the correct absolute Haiku paths instead.
if(NOT TARGET avif)
  add_library(avif SHARED IMPORTED)
  set_target_properties(avif PROPERTIES
    IMPORTED_LOCATION "/boot/system/lib/libavif.so.13.0.0"
    INTERFACE_INCLUDE_DIRECTORIES "/boot/system/develop/headers")
endif()
set(LIBAVIF_FOUND TRUE)
