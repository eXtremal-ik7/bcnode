include(ExternalProject)

set(CONFIG4CPP_DIRECTORY "${CMAKE_SOURCE_DIR}/../dependencies/config4cpp")

if (NOT MSVC)
  ExternalProject_Add(config4cpp_project
    GIT_REPOSITORY https://github.com/eXtremal-ik7/config4cpp
    GIT_TAG master
    GIT_SHALLOW 1
    SOURCE_DIR ${CONFIG4CPP_DIRECTORY}
    BINARY_DIR ${CONFIG4CPP_DIRECTORY}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND make
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
    BUILD_BYPRODUCTS ${CONFIG4CPP_DIRECTORY}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}config4cpp${CMAKE_STATIC_LIBRARY_SUFFIX}
  )
else()
  ExternalProject_Add(config4cpp_project
    GIT_REPOSITORY https://github.com/eXtremal-ik7/config4cpp
    GIT_TAG master
    GIT_SHALLOW 1
    SOURCE_DIR ${CONFIG4CPP_DIRECTORY}
    BINARY_DIR ${CONFIG4CPP_DIRECTORY}/src
    CONFIGURE_COMMAND ""
    BUILD_COMMAND nmake -f Makefile.win config4cpp.lib
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
    BUILD_BYPRODUCTS ${CONFIG4CPP_DIRECTORY}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}config4cpp${CMAKE_STATIC_LIBRARY_SUFFIX}
  )
endif()

add_library(config4cpp STATIC IMPORTED)
add_dependencies(config4cpp config4cpp_project)
set_target_properties(config4cpp PROPERTIES IMPORTED_LOCATION ${CONFIG4CPP_DIRECTORY}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}config4cpp${CMAKE_STATIC_LIBRARY_SUFFIX})
set(CONFIG4CPP_INCLUDE_DIR ${CONFIG4CPP_DIRECTORY}/include)
