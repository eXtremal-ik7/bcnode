include(FetchContent)

# Hashes, PoW and fixed-width integers. ISA paths are libpow's own options (LIBPOW_SHA_NI,
# LIBPOW_ARM_CRYPTO, LIBPOW_SCRYPT_FORCE_SCALAR), set on the bcnode command line
FetchContent_Declare(
  libpow
  GIT_REPOSITORY https://github.com/eXtremal-ik7/libpow.git
  GIT_TAG        main
  GIT_SHALLOW    1
  SOURCE_DIR     ${CMAKE_SOURCE_DIR}/../dependencies/libpow
)

FetchContent_GetProperties(libpow)
if (NOT libpow_POPULATED)
  FetchContent_Populate(libpow)
  add_subdirectory(${libpow_SOURCE_DIR}/src ${libpow_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
