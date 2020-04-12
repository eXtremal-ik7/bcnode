include(ExternalProject)

set(CONFIG4CPP_DIRECTORY "${CMAKE_BINARY_DIR}/dependencies/config4cpp")

if (NOT MSVC)
  ExternalProject_Add(config4cpp
    GIT_REPOSITORY https://github.com/eXtremal-ik7/config4cpp
    GIT_TAG master
    GIT_SHALLOW 1
    SOURCE_DIR ${CONFIG4CPP_DIRECTORY}
    BINARY_DIR ${CONFIG4CPP_DIRECTORY}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_MAKE_PROGRAM}
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
  )
else()
  ExternalProject_Add(config4cpp
    GIT_REPOSITORY https://github.com/eXtremal-ik7/config4cpp
    GIT_TAG master
    GIT_SHALLOW 1
    SOURCE_DIR ${CONFIG4CPP_DIRECTORY}
    BINARY_DIR ${CONFIG4CPP_DIRECTORY}/src
    CONFIGURE_COMMAND ""
    BUILD_COMMAND nmake -f Makefile.win config4cpp.lib
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
  )
endif()

add_library(CONFIG4CPP_LIBRARY__ STATIC IMPORTED DEPENDS config4cpp)
set_target_properties(CONFIG4CPP_LIBRARY__ PROPERTIES IMPORTED_LOCATION ${CONFIG4CPP_DIRECTORY}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}config4cpp${CMAKE_STATIC_LIBRARY_SUFFIX})

set(CONFIG4CPP_INCLUDE_DIR ${CONFIG4CPP_DIRECTORY}/include)
set(CONFIG4CPP_LIBRARY CONFIG4CPP_LIBRARY__)
