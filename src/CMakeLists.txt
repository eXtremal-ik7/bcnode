include("cmake/cxx-pm.cmake")
cxxpm_initialize(
  https://github.com/eXtremal-ik7/cxx-pm/archive/refs/tags/0.0.5.tar.gz
  d520a56610477f1911d1a390775e2ce19c47dfaa098d9692de5340cb174e3c43
)

cmake_minimum_required(VERSION 3.21)
project(bcnode)

set (CMAKE_CXX_STANDARD 20)
option(BCNODE_DOGE "Build bcnode for dogecoin" OFF)
option(BCNODE_ZEC "Build bcnode for ZCash" OFF)

# Whole-program build modes. The names are shared by convention across projects in one
# build tree (cache is common, so the same option here and in asyncio merges into a single
# switch); each project applies the flags to its own scope.
option(BUILD_SANITIZE_ADDRESS "Build with address sanitizer" OFF)
option(BUILD_SANITIZE_THREAD "Build with thread sanitizer" OFF)
option(BUILD_PROFILING "Build for profiling" OFF)
option(BUILD_WITH_ASSERTS "Keep asserts in Release/RelWithDebInfo" OFF)

if (BUILD_SANITIZE_ADDRESS AND BUILD_SANITIZE_THREAD)
  message(FATAL_ERROR "BUILD_SANITIZE_ADDRESS and BUILD_SANITIZE_THREAD are mutually exclusive")
endif()

if (BUILD_SANITIZE_ADDRESS)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
endif()

if (BUILD_SANITIZE_THREAD)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=thread")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
endif()

if (BUILD_PROFILING)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -fno-inline -fno-omit-frame-pointer")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-inline -fno-omit-frame-pointer")
endif()

if (BUILD_WITH_ASSERTS)
  add_definitions(-UNDEBUG)
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
endif()

set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH}
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake
)

add_definitions(-DTBB_USE_EXCEPTIONS=0)

if(MSVC)
  add_definitions(
    -D_CRT_SECURE_NO_DEPRECATE
    -D_CRT_SECURE_NO_WARNINGS
    -D_CRT_NONSTDC_NO_DEPRECATE
    -D__STDC_LIMIT_MACROS
    -D__STDC_FORMAT_MACROS
    -DNOMINMAX
    -DNOGDI
  )

  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GR-")
  # getopt only; the c99 shims live in asyncio and are added below, after it is populated
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/msvc/include)
  set(GETOPT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/msvc/getopt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/msvc/getopt1.c
  )
else(MSVC)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -fno-rtti")
  if (CXXPM_SYSTEM_PROCESSOR STREQUAL "x86_64")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcx16")
  endif()
endif(MSVC)

cxxpm_add_package(one-tbb default)
cxxpm_add_package(rocksdb default)
cxxpm_add_package(rapidjson default)
if (NOT MSVC)
  cxxpm_add_package(gmp default)
  set(BIGNUM_LIBRARIES gmp::libgmp gmp::libgmpxx)
else()
  cxxpm_add_package(mpir default)
  set(BIGNUM_LIBRARIES ${MPIR_LIBRARIES})
endif()

# The modules below drive FetchContent by hand: asyncio is added from a non-default source
# subdirectory and all three need EXCLUDE_FROM_ALL, which FetchContent_MakeAvailable only
# supports from CMake 3.28. Keep FetchContent_Populate until the minimum version is raised.
if (POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

include(${CMAKE_SOURCE_DIR}/cmake/ProjectConfig4cpp.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/ProjectAsyncio.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/ProjectSecp256k1.cmake)

include_directories(
  ${CMAKE_SOURCE_DIR}
  ${asyncio_SOURCE_DIR}/src/include
  ${asyncio_BINARY_DIR}/include
  ${config4cpp_SOURCE_DIR}/src/include
  ${secp256k1_SOURCE_DIR}/include
  ${RAPIDJSON_INCLUDE_DIRECTORY}
)

if (MSVC AND MSVC_VERSION LESS 1800)
  # inttypes.h/stdint.h appeared in VS2013; older compilers use the shims bundled with
  # asyncio, for newer ones the shims must not shadow the system headers
  include_directories(${asyncio_SOURCE_DIR}/src/msvc/include/c99)
endif()

add_library(bcnodecommon STATIC
  loguru.cpp
  common/base58.cpp
  common/file.cpp
  common/serializedDataCache.cpp
  common/utils.cpp
)

target_link_libraries(bcnodecommon
  ${BIGNUM_LIBRARIES}
)

option(LIBPOW_SSE2_ENABLED off)
option(LIBPOW_SHANI_ENABLED off)
option(LIBPOW_ARM_SHA2_ENABLED off)
option(LIBPOW_SCRYPT_SSE2_DISABLED off)

if (LIBPOW_SHANI_ENABLED)
  add_definitions(-DLIBPOW_SHANI_ENABLED)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -msha -msse4.1")
endif()

if (LIBPOW_ARM_SHA2_ENABLED)
  add_definitions(-DLIBPOW_ARM_SHA2_ENABLED)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=armv8-a+crypto")
endif()

if (LIBPOW_SSE2_ENABLED)
  add_definitions(-DLIBPOW_SSE2_ENABLED)
endif()

if (LIBPOW_SCRYPT_SSE2_DISABLED)
  add_definitions(-DLIBPOW_SCRYPT_SSE2_DISABLED)
endif()

add_library(bcnodecrypto STATIC
  crypto/scrypt.c
  crypto/sha256.c
)

target_link_libraries(bcnodecrypto
  OpenSSL::SSL
  OpenSSL::Crypto
)

set(BASE_SOURCES
  bcnode.cpp
  common/blockDataBase.cpp
  common/blockSource.cpp
  common/linearDataStorage.cpp
  common/merkleTree.cpp
  common/thread.cpp
  db/archive.cpp
  db/addrHistoryDb.cpp
  db/storage.cpp
  db/txdb.cpp
  db/txdbRef.cpp
  db/utxodb.cpp
  db/sync.cpp
)

# Linker configuration
set(COMMON_LIBRARIES
  bcnodecommon
  bcnodecrypto
  asyncio-0.6
  asyncioextras-0.6
  p2putils
  config4cpp
  secp256k1
  TBB::tbb
  OpenSSL::SSL
  OpenSSL::Crypto
  RocksDB::rocksdb
)

if (MSVC)
  add_library(getopt STATIC
    ${CMAKE_SOURCE_DIR}/msvc/getopt.c
    ${CMAKE_SOURCE_DIR}/msvc/getopt1.c
  )

  set(COMMON_LIBRARIES ${COMMON_LIBRARIES} getopt)
endif()

# OS-specific configuration
if (WIN32)
  # Winsock libraries
  set(COMMON_LIBRARIES ${COMMON_LIBRARIES} ws2_32 mswsock shlwapi crypt32 rpcrt4)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Add dynamic libraries support
  # Use custom allocator "jemalloc"
  set(COMMON_LIBRARIES ${COMMON_LIBRARIES} dl jemalloc rt)
endif()

# Compiler-specific configuration

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(COMMON_LIBRARIES ${COMMON_LIBRARIES}
    atomic
    pthread
    stdc++fs
  )
endif()

macro(prepare_bc_env NAME)
  make_directory(${CMAKE_BINARY_DIR}/${NAME}/BC)

  list(APPEND BC_FILES
    "connection.h"
    "http.h"
    "nativeApi.h"
    "network.h"
    "proto.h"
    "serialize.h"
    "script.h"
  )

  foreach(FILE IN LISTS BC_FILES)
    file(WRITE ${CMAKE_BINARY_DIR}/${NAME}/BC/${FILE} "#include \"${CMAKE_SOURCE_DIR}/${NAME}/${FILE}\"")
  endforeach()

  # Main inclide file
  string(TOLOWER ${NAME} NAME_LOWER_CASE)
  file(WRITE ${CMAKE_BINARY_DIR}/${NAME}/BC/bc.h "#include \"${CMAKE_SOURCE_DIR}/${NAME}/${NAME_LOWER_CASE}.h\"")
endmacro()


# BTC
prepare_bc_env(BTC)

set(BTC_SOURCES
  BTC/proto.cpp
  BTC/network.cpp
  BTC/http.cpp
  BTC/nativeApi.cpp
  BTC/script.cpp
)

add_executable(bcnode-btc
  ${BASE_SOURCES}
  ${BTC_SOURCES}
  BTC/btc.cpp
)

add_executable(bcterminal-btc
  bcterminal.cpp
  BTC/btc.cpp
  BTC/proto.cpp
  common/merkleTree.cpp
)

add_executable(bctest-btc
  bctest.cpp
  BTC/btc.cpp
  BTC/proto.cpp
  common/merkleTree.cpp
)

target_include_directories(bcnode-btc PUBLIC ${CMAKE_BINARY_DIR}/BTC)
target_compile_definitions(bcnode-btc PUBLIC -DBC=BTC)
target_link_libraries(bcnode-btc ${COMMON_LIBRARIES})

target_include_directories(bcterminal-btc PUBLIC ${CMAKE_BINARY_DIR}/BTC)
target_compile_definitions(bcterminal-btc PUBLIC -DBC=BTC)
target_link_libraries(bcterminal-btc ${COMMON_LIBRARIES})

target_include_directories(bctest-btc PUBLIC ${CMAKE_BINARY_DIR}/BTC)
target_compile_definitions(bctest-btc PUBLIC -DBC=BTC)
target_link_libraries(bctest-btc ${COMMON_LIBRARIES})

# LTC
prepare_bc_env(LTC)

add_executable(bcnode-ltc
  ${BASE_SOURCES}
  ${BTC_SOURCES}
  BTC/btc.cpp
  LTC/ltc.cpp
  LTC/proto.cpp
)

add_executable(bcterminal-ltc
  bcterminal.cpp
  BTC/proto.cpp
  LTC/ltc.cpp
  LTC/proto.cpp
  common/merkleTree.cpp
)

target_include_directories(bcnode-ltc PUBLIC ${CMAKE_BINARY_DIR}/LTC)
target_compile_definitions(bcnode-ltc PUBLIC -DBC=LTC)
target_link_libraries(bcnode-ltc ${COMMON_LIBRARIES})

target_include_directories(bcterminal-ltc PUBLIC ${CMAKE_BINARY_DIR}/LTC)
target_compile_definitions(bcterminal-ltc PUBLIC -DBC=LTC)
target_link_libraries(bcterminal-ltc ${COMMON_LIBRARIES})

# XPM
prepare_bc_env(XPM)

add_executable(bcnode-xpm
  ${BASE_SOURCES}
  ${BTC_SOURCES}
  XPM/xpm.cpp
  XPM/proto.cpp
)

add_executable(bcterminal-xpm
  bcterminal.cpp
  BTC/proto.cpp
  XPM/xpm.cpp
  XPM/proto.cpp
  common/merkleTree.cpp
)

target_include_directories(bcnode-xpm PUBLIC ${CMAKE_BINARY_DIR}/XPM ${MPIR_INCLUDE_DIR})
target_compile_definitions(bcnode-xpm PUBLIC -DBC=XPM)
target_link_libraries(bcnode-xpm ${COMMON_LIBRARIES})

target_include_directories(bcterminal-xpm PUBLIC ${CMAKE_BINARY_DIR}/XPM)
target_compile_definitions(bcterminal-xpm PUBLIC -DBC=XPM)
target_link_libraries(bcterminal-xpm ${COMMON_LIBRARIES})

# DOGE
if (BCNODE_DOGE)
  prepare_bc_env(DOGE)

  add_executable(bcnode-doge
    ${BASE_SOURCES}
    ${BTC_SOURCES}
    LTC/ltc.cpp
    LTC/proto.cpp
    DOGE/doge.cpp
    DOGE/proto.cpp
    DOGE/validation.cpp
  )

  add_executable(bcterminal-doge
    bcterminal.cpp
    LTC/ltc.cpp
    LTC/proto.cpp
    DOGE/doge.cpp
    DOGE/proto.cpp
    DOGE/validation.cpp
    BTC/proto.cpp
    common/merkleTree.cpp
  )

  target_include_directories(bcnode-doge PUBLIC ${CMAKE_BINARY_DIR}/DOGE)
  target_compile_definitions(bcnode-doge PUBLIC -DBC=DOGE)
  target_link_libraries(bcnode-doge ${COMMON_LIBRARIES})

  target_include_directories(bcterminal-doge PUBLIC ${CMAKE_BINARY_DIR}/DOGE)
  target_compile_definitions(bcterminal-doge PUBLIC -DBC=DOGE)
  target_link_libraries(bcterminal-doge ${COMMON_LIBRARIES})
endif()

# ZEC
if (BCNODE_ZEC)
  prepare_bc_env(ZEC)

  add_executable(bcnode-zec
    ${BASE_SOURCES}
    ${BTC_SOURCES}
    ZEC/zec.cpp
    ZEC/proto.cpp
  )

  add_executable(bcterminal-zec
    bcterminal.cpp
    ZEC/zec.cpp
    ZEC/proto.cpp
    BTC/proto.cpp
    common/merkleTree.cpp
  )

  target_include_directories(bcnode-zec PUBLIC ${CMAKE_BINARY_DIR}/ZEC)
  target_compile_definitions(bcnode-zec PUBLIC -DBC=ZEC)
  target_link_libraries(bcnode-zec ${COMMON_LIBRARIES})

  target_include_directories(bcterminal-zec PUBLIC ${CMAKE_BINARY_DIR}/ZEC)
  target_compile_definitions(bcterminal-zec PUBLIC -DBC=ZEC)
  target_link_libraries(bcterminal-zec ${COMMON_LIBRARIES})
endif()

# Windows has no rpath: DLLs of imported shared packages (mpir and the like) are
# looked up through PATH at run time, so drop them next to the executable instead.
# The generator expression picks the DLL matching the configuration being built.
if (WIN32)
  get_property(BCNODE_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach (BCNODE_TARGET IN LISTS BCNODE_TARGETS)
    get_target_property(BCNODE_TARGET_TYPE ${BCNODE_TARGET} TYPE)
    if (BCNODE_TARGET_TYPE STREQUAL "EXECUTABLE")
      add_custom_command(TARGET ${BCNODE_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_RUNTIME_DLLS:${BCNODE_TARGET}> $<TARGET_FILE_DIR:${BCNODE_TARGET}>
        COMMAND_EXPAND_LISTS
      )
    endif()
  endforeach()
endif()
