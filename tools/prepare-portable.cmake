# Supply the default INI only. Pairing state is created by runtime discovery.
if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_DIR)
  message(FATAL_ERROR "SOURCE_DIR and OUTPUT_DIR are required")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
if(NOT EXISTS "${OUTPUT_DIR}/config.ini")
  configure_file("${SOURCE_DIR}/config.ini" "${OUTPUT_DIR}/config.ini" COPYONLY)
endif()
