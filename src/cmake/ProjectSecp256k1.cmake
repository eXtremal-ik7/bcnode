include(ExternalProject)

set(SECP256K1_SOURCE_DIRECTORY "${CMAKE_SOURCE_DIR}/../dependencies/secp256k1")
set(SECP256K1_BUILD_DIRECTORY "${CMAKE_SOURCE_DIR}/../dependencies/secp256k1-${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_SYSTEM_NAME}")

ExternalProject_Add(secp256k1
  GIT_REPOSITORY https://github.com/bitcoin-core/secp256k1
  GIT_TAG 39198a03eaa33d5902b16d3aefa7c441232f60fb
  SOURCE_DIR ${SECP256K1_SOURCE_DIRECTORY}
  BINARY_DIR ${SECP256K1_BUILD_DIRECTORY}
  PATCH_COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cmake/secp256k1/CMakeLists.txt <SOURCE_DIR> 
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/cmake/secp256k1/libsecp256k1-config.h.cmake.in <SOURCE_DIR>/src
  INSTALL_COMMAND ""
  UPDATE_COMMAND ""
)

if (MSVC)
  add_library(SECP256K1_LIBRARY_DEBUG     STATIC IMPORTED DEPENDS secp256k1)
  add_library(SECP256K1_LIBRARY_OPTIMIZED STATIC IMPORTED DEPENDS secp256k1)
  set_target_properties(SECP256K1_LIBRARY_DEBUG     PROPERTIES IMPORTED_LOCATION ${SECP256K1_BUILD_DIRECTORY}/Debug/secp256k1.lib)
  set_target_properties(SECP256K1_LIBRARY_OPTIMIZED PROPERTIES IMPORTED_LOCATION ${SECP256K1_BUILD_DIRECTORY}/Release/secp256k1.lib)
  set(SECP256K1_LIBRARY debug SECP256K1_LIBRARY_DEBUG optimized SECP256K1_LIBRARY_OPTIMIZED)
else()
  add_library(SECP256K1_LIBRARY__ STATIC IMPORTED DEPENDS secp256k1)
  set_target_properties(SECP256K1_LIBRARY__ PROPERTIES IMPORTED_LOCATION ${SECP256K1_BUILD_DIRECTORY}/${CMAKE_STATIC_LIBRARY_PREFIX}secp256k1{CMAKE_STATIC_LIBRARY_SUFFIX})
  set(SECP256K1_LIBRARY CONFIG4CPP_LIBRARY__)
endif()

set(SECP256K1_INCLUDE_DIR ${SECP256K1_SOURCE_DIRECTORY}/include)
