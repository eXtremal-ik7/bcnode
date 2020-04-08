set(BUILD_DIR "${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_SYSTEM_NAME}")

if (ROOT_SOURCE_DIR)
  set(PATHS
    ${ROOT_SOURCE_DIR}/libp2p/${BUILD_DIR}
  )

  if (MSVC)
    find_library(ASYNCIO_LIBRARY_DEBUG asyncio-0.5${CMAKE_DEBUG_POSTFIX}
      PATHS ${PATHS}
      PATH_SUFFIXES asyncio/Debug
    )
    find_library(ASYNCIO_LIBRARY_OPTIMIZED asyncio-0.5
      PATHS ${PATHS}
      PATH_SUFFIXES asyncio/Release
    )
    set(ASYNCIO_LIBRARY debug ${ASYNCIO_LIBRARY_DEBUG} optimized ${ASYNCIO_LIBRARY_OPTIMIZED})
  else()
    find_library(ASYNCIO_LIBRARY asyncio-0.5
      PATHS ${PATHS}
      PATH_SUFFIXES asyncio
    )
  endif()
  message("-- Found asyncio library at ${ASYNCIO_LIBRARY}")

  if (MSVC)
    find_library(ASYNCIO_EXTRAS_LIBRARY_DEBUG asyncioextras-0.5${CMAKE_DEBUG_POSTFIX}
      PATHS ${PATHS}
      PATH_SUFFIXES asyncioextras/Debug
    )
    find_library(ASYNCIO_EXTRAS_LIBRARY_OPTIMIZED asyncioextras-0.5
      PATHS ${PATHS}
      PATH_SUFFIXES asyncioextras/Release
    )
    set(ASYNCIO_EXTRAS_LIBRARY debug ${ASYNCIO_EXTRAS_LIBRARY_DEBUG} optimized ${ASYNCIO_EXTRAS_LIBRARY_OPTIMIZED})
  else()
    find_library(ASYNCIO_EXTRAS_LIBRARY asyncioextras-0.5
      PATHS ${PATHS}
      PATH_SUFFIXES asyncioextras
    )
  endif()
  message("-- Found asyncioextras library at ${ASYNCIO_EXTRAS_LIBRARY}")

  if (MSVC)
    find_library(P2P_LIBRARY_DEBUG p2p${CMAKE_DEBUG_POSTFIX}
      PATHS ${PATHS}
      PATH_SUFFIXES p2p/Debug
    )
    find_library(P2P_LIBRARY_OPTIMIZED p2p
      PATHS ${PATHS}
      PATH_SUFFIXES p2p/Release
    )
    set(P2P_LIBRARY debug ${P2P_LIBRARY_DEBUG} optimized ${P2P_LIBRARY_OPTIMIZED})
  else()
    find_library(P2P_LIBRARY p2p
      PATHS ${PATHS}
      PATH_SUFFIXES p2p
    )
  endif()
  message("-- Found p2p library at ${P2P_LIBRARY}")

  if (MSVC)
    find_library(P2PUTILS_LIBRARY_DEBUG p2putils${CMAKE_DEBUG_POSTFIX}
      PATHS ${PATHS}
      PATH_SUFFIXES p2putils/Debug
    )
    find_library(P2PUTILS_LIBRARY_OPTIMIZED p2putils
      PATHS ${PATHS}
      PATH_SUFFIXES p2putils/Release
    )
    set(P2PUTILS_LIBRARY debug ${P2PUTILS_LIBRARY_DEBUG} optimized ${P2PUTILS_LIBRARY_OPTIMIZED})
  else()
    find_library(P2PUTILS_LIBRARY p2putils
      PATHS ${PATHS}
      PATH_SUFFIXES p2putils
    )
  endif()
  message("-- Found p2putils library at ${P2PUTILS_LIBRARY}")

  find_path(ASYNCIO_INCLUDE_DIR "asyncio/asyncio.h"
    PATH ${ROOT_SOURCE_DIR}/libp2p/src/include
  )

  find_path(ASYNCIO_INCLUDE_CONFIG_DIR "config.h"
    PATHS ${PATHS}
    PATH_SUFFIXES include
  )
endif()

set(ASYNCIO_INCLUDE_DIR ${ASYNCIO_INCLUDE_DIR} ${ASYNCIO_INCLUDE_CONFIG_DIR})

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(ASYNCIO_LIBRARY ${ASYNCIO_LIBRARY} rt)
endif()
