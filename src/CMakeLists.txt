cmake_minimum_required(VERSION 3.10)
project(bcnode)

set (CMAKE_CXX_STANDARD 17)
option(SANITIZER_ENABLED "Build with address sanitizer" OFF)
option(GPROF_ENABLED "Build with GNU profiler (use gprof ./exename -p > out.txt)" OFF)

if (SANITIZER_ENABLED)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
endif()

if (GPROF_ENABLED)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pg")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pg")
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
endif()

set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH}
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake
)

set(CMAKE_DEBUG_POSTFIX d)

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

  include_directories(${CMAKE_SOURCE_DIR}/msvc/include)
else(MSVC)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcx16")
endif(MSVC)

find_package(OpenSSL REQUIRED)
find_package(libp2p REQUIRED)
find_package(config4cpp REQUIRED)
find_package(mpir REQUIRED)

set(_tbb_cmake_module_path ${CMAKE_SOURCE_DIR}/cmake)
include(${_tbb_cmake_module_path}/TBBGet.cmake)
include(${_tbb_cmake_module_path}/TBBBuild.cmake)
tbb_get(TBB_ROOT tbb_root SOURCE_CODE)
tbb_build(TBB_ROOT ${tbb_root} CONFIG_DIR TBB_DIR)
find_package(TBB REQUIRED tbb)

add_library(bcnodecommon STATIC
  loguru.cpp
  common/arith_uint256.cpp
  common/bigNum.cpp
  common/serializedDataCache.cpp
  common/uint256.cpp
)

add_library(bcnodecrypto STATIC
  crypto/scrypt.cpp
)

set(BASE_SOURCES
  bcnode.cpp
  common/blockDataBase.cpp
  common/blockSource.cpp
  common/linearDataStorage.cpp
  common/merkleTree.cpp
  common/thread.cpp
)

if (MSVC)
  set(BASE_SOURCES ${BASE_SOURCES}
    ${CMAKE_SOURCE_DIR}/msvc/getopt.c
    ${CMAKE_SOURCE_DIR}/msvc/getopt1.c
  )
endif()

# Linker configuration

set (COMMON_LIBRARIES
  bcnodecommon
  ${OPENSSL_LIBRARIES}
  ${ASYNCIO_LIBRARY}
  ${ASYNCIO_EXTRAS_LIBRARY}
  ${P2PUTILS_LIBRARY}
  ${CONFIG4CPP_LIBRARY}
  ${MPIR_LIBRARY}
  ${MPIR_CXX_LIBRARY}
  ${TBB_IMPORTED_TARGETS}
)

# OS-specific configuration
if (WIN32)
  # Winsock libraries
  set(COMMON_LIBRARIES ${COMMON_LIBRARIES} ws2_32 mswsock)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Add dynamic libraries support
  # Use custom allocator "jemalloc"
  set(COMMON_LIBRARIES ${COMMON_LIBRARIES} dl jemalloc)
endif()

# Compiler-specific configuration

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(COMMON_LIBRARIES ${COMMON_LIBRARIES}
    atomic
    pthread
    stdc++fs
  )
endif()

include_directories(
  ${CMAKE_SOURCE_DIR}
  ${ASYNCIO_INCLUDE_DIR}
  ${CONFIG4CPP_INCLUDE_DIR}
  ${MPIR_INCLUDE_DIR}
)

macro(prepare_bc_env NAME)
  make_directory(${CMAKE_BINARY_DIR}/${NAME}/BC)

  list(APPEND BC_FILES
    "http.h"
    "nativeApi.h"
    "network.h"
    "proto.h"
    "serialize.h"
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
)

add_executable(bcnode-btc
  ${BASE_SOURCES}
  ${BTC_SOURCES}
  BTC/btc.cpp
)

target_include_directories(bcnode-btc PUBLIC ${CMAKE_BINARY_DIR}/BTC)
target_compile_definitions(bcnode-btc PUBLIC -DBC=BTC)
target_link_libraries(bcnode-btc ${COMMON_LIBRARIES})

# LTC
prepare_bc_env(LTC)

add_executable(bcnode-ltc
  ${BASE_SOURCES}
  ${BTC_SOURCES}
  LTC/ltc.cpp
)

target_include_directories(bcnode-ltc PUBLIC ${CMAKE_BINARY_DIR}/LTC)
target_compile_definitions(bcnode-ltc PUBLIC -DBC=LTC)
target_link_libraries(bcnode-ltc ${COMMON_LIBRARIES} bcnodecrypto)

# XPM
prepare_bc_env(XPM)

add_executable(bcnode-xpm
  ${BASE_SOURCES}
  ${BTC_SOURCES}
  XPM/xpm.cpp
  XPM/proto.cpp
)

target_include_directories(bcnode-xpm PUBLIC ${CMAKE_BINARY_DIR}/XPM ${MPIR_INCLUDE_DIR})
target_compile_definitions(bcnode-xpm PUBLIC -DBC=XPM)
target_link_libraries(bcnode-xpm ${COMMON_LIBRARIES})
