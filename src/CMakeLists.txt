include("cmake/cxx-pm.cmake")
cxxpm_initialize(
  https://github.com/eXtremal-ik7/cxx-pm/archive/refs/tags/0.0.4.tar.gz
  409fdb16b267926d0cae526041f6b6174fe17b7170519e3d35ae2486e9139f1a
)

cmake_minimum_required(VERSION 3.11)
project(bcnode)

set (CMAKE_CXX_STANDARD 20)
option(SANITIZER_ENABLED "Build with address sanitizer" OFF)
option(PROFILE_ENABLED "Build for profiling" OFF)
option(ASSERTS_ENABLED "Build Release/RelWithDebInfo with asserts" OFF)
option(BCNODE_DOGE "Build bcnode for dogecoin" OFF)
option(BCNODE_ZEC "Build bcnode for ZCash" OFF)

if (SANITIZER_ENABLED)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
endif()

if (PROFILE_ENABLED)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -fno-inline -fno-omit-frame-pointer")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fno-inline -fno-omit-frame-pointer")
endif()

if (NOT ASSERTS_ENABlED)
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

include(${CMAKE_SOURCE_DIR}/cmake/ProjectConfig4cpp.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/ProjectLibp2p.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/ProjectSecp256k1.cmake)

include_directories(
  ${CMAKE_SOURCE_DIR}
  ${libp2p_SOURCE_DIR}/src/include
  ${libp2p_BINARY_DIR}/include
  ${config4cpp_SOURCE_DIR}/src/include
  ${secp256k1_SOURCE_DIR}/include
  ${RAPIDJSON_INCLUDE_DIRECTORY}
)

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

if (LIBPOW_SHANI_ENABLED)
  add_definitions(-DLIBPOW_SHANI_ENABLED)
  set_source_files_properties(sha256.c PROPERTIES COMPILE_FLAGS -march=znver1)
endif()

if (LIBPOW_SSE2_ENABLED)
  add_definitions(-DLIBPOW_SSE2_ENABLED)
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
  asyncio-0.5
  asyncioextras-0.5
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
target_link_libraries(bcnode-ltc ${COMMON_LIBRARIES} bcnodecrypto)

target_include_directories(bcterminal-ltc PUBLIC ${CMAKE_BINARY_DIR}/LTC)
target_compile_definitions(bcterminal-ltc PUBLIC -DBC=LTC)
target_link_libraries(bcterminal-ltc ${COMMON_LIBRARIES} bcnodecrypto)

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
    DOGE/doge.cpp
    DOGE/proto.cpp
    DOGE/validation.cpp
  )

  add_executable(bcterminal-doge
    bcterminal.cpp
    LTC/ltc.cpp
    DOGE/doge.cpp
    DOGE/proto.cpp
    DOGE/validation.cpp
    BTC/proto.cpp
    common/merkleTree.cpp
  )

  target_include_directories(bcnode-doge PUBLIC ${CMAKE_BINARY_DIR}/DOGE)
  target_compile_definitions(bcnode-doge PUBLIC -DBC=DOGE)
  target_link_libraries(bcnode-doge ${COMMON_LIBRARIES} bcnodecrypto)

  target_include_directories(bcterminal-doge PUBLIC ${CMAKE_BINARY_DIR}/DOGE)
  target_compile_definitions(bcterminal-doge PUBLIC -DBC=DOGE)
  target_link_libraries(bcterminal-doge ${COMMON_LIBRARIES} bcnodecrypto)
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
  target_link_libraries(bcnode-zec ${COMMON_LIBRARIES} bcnodecrypto)

  target_include_directories(bcterminal-zec PUBLIC ${CMAKE_BINARY_DIR}/ZEC)
  target_compile_definitions(bcterminal-zec PUBLIC -DBC=ZEC)
  target_link_libraries(bcterminal-zec ${COMMON_LIBRARIES} bcnodecrypto)
endif()
