#----------------------------------------------------------------
# Generated CMake target import file for configuration "None".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "cjson_utils" for configuration "None"
set_property(TARGET cjson_utils APPEND PROPERTY IMPORTED_CONFIGURATIONS NONE)
set_target_properties(cjson_utils PROPERTIES
  IMPORTED_LOCATION_NONE "/usr/lib/x86_64-linux-gnu/libcjson_utils.so.1.7.19"
  IMPORTED_SONAME_NONE "libcjson_utils.so.1"
  )

list(APPEND _cmake_import_check_targets cjson_utils )
list(APPEND _cmake_import_check_files_for_cjson_utils "/usr/lib/x86_64-linux-gnu/libcjson_utils.so.1.7.19" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
