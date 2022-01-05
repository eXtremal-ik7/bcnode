include(FetchContent)

FetchContent_Declare(
  config4cpp
  GIT_REPOSITORY https://github.com/eXtremal-ik7/config4cpp
  GIT_TAG        master
  GIT_SHALLOW    1
  SOURCE_DIR     ${CMAKE_SOURCE_DIR}/../dependencies/config4cpp
)

FetchContent_GetProperties(config4cpp)
if (NOT config4cpp_POPULATED)
  FetchContent_Populate(config4cpp)
  add_subdirectory(${config4cpp_SOURCE_DIR} ${config4cpp_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
