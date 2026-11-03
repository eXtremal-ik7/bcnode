include(FetchContent)

# A static library with what script checks need: ECDSA, extrakeys and schnorrsig for taproot
set(SECP256K1_ENABLE_MODULE_ECDH OFF CACHE INTERNAL "")
set(SECP256K1_ENABLE_MODULE_RECOVERY OFF CACHE INTERNAL "")
set(SECP256K1_ENABLE_MODULE_EXTRAKEYS ON CACHE INTERNAL "")
set(SECP256K1_ENABLE_MODULE_SCHNORRSIG ON CACHE INTERNAL "")
set(SECP256K1_ENABLE_MODULE_MUSIG OFF CACHE INTERNAL "")
set(SECP256K1_ENABLE_MODULE_ELLSWIFT OFF CACHE INTERNAL "")
set(SECP256K1_ENABLE_MODULE_SILENTPAYMENTS OFF CACHE INTERNAL "")
# Portable C on every platform, as before; valgrind support would depend on the host
set(SECP256K1_ASM OFF CACHE INTERNAL "")
set(SECP256K1_VALGRIND OFF CACHE INTERNAL "")
set(SECP256K1_BUILD_BENCHMARK OFF CACHE INTERNAL "")
set(SECP256K1_BUILD_TESTS OFF CACHE INTERNAL "")
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS OFF CACHE INTERNAL "")
set(SECP256K1_BUILD_CTIME_TESTS OFF CACHE INTERNAL "")

# Not dependencies/secp256k1: an existing build updates that old checkout in place and fails,
# git can't restore our CMakeLists.txt copied there over the one v0.8.0 tracks
FetchContent_Declare(secp256k1
  GIT_REPOSITORY https://github.com/bitcoin-core/secp256k1
  GIT_TAG        6e2c8bc4ecdc6e71dbe7a368f360d8d453ce435d # v0.8.0
  SOURCE_DIR     ${CMAKE_SOURCE_DIR}/../dependencies/libsecp256k1
)

FetchContent_GetProperties(secp256k1)
if (NOT secp256k1_POPULATED)
  FetchContent_Populate(secp256k1)
  add_subdirectory(${secp256k1_SOURCE_DIR} ${secp256k1_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
