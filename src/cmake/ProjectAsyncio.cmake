include(FetchContent)

# asyncio protocol layers bcnode does not use (ZMTP additionally pulls in zeromq)
set(ASYNCIO_ENABLE_ZMTP OFF CACHE INTERNAL "")
set(ASYNCIO_ENABLE_RLPX OFF CACHE INTERNAL "")

FetchContent_Declare(
  asyncio
  GIT_REPOSITORY https://github.com/eXtremal-ik7/asyncio.git
  GIT_TAG        master
  GIT_SHALLOW    1
  SOURCE_DIR     ${CMAKE_SOURCE_DIR}/../dependencies/asyncio
)

FetchContent_GetProperties(asyncio)
if (NOT asyncio_POPULATED)
  FetchContent_Populate(asyncio)
  add_subdirectory(${asyncio_SOURCE_DIR}/src ${asyncio_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
