if ( APPLE )
  cmake_minimum_required( VERSION 3.13 )
else ()
  cmake_minimum_required( VERSION 3.5 )
endif ()

project( uxplay )

message( STATUS "Project name: " ${PROJECT_NAME} )

include(GNUInstallDirs)

set ( CMAKE_CXX_STANDARD 11 )

find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
include_directories(${AVCODEC_INCLUDE_DIRS} ${AVUTIL_INCLUDE_DIRS} ${AVFORMAT_INCLUDE_DIRS})


if( UNIX AND NOT APPLE )
 add_definitions( -DSUPPRESS_AVAHI_COMPAT_WARNING )
else()
  set( CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE )
endif()

add_subdirectory( lib/llhttp )
add_subdirectory( lib/playfair )
add_subdirectory( lib )
add_subdirectory( renderers )

if  ( GST_MACOS )
     add_definitions( -DGST_MACOS )
     message ( STATUS "define GST_MACOS" )
endif()

add_executable( uxplay uxplay.cpp )
target_link_libraries( uxplay
                   renderers
                   airplay
                   ${AVCODEC_LIBRARIES}
                   ${AVUTIL_LIBRARIES}
                   ${AVFORMAT_LIBRARIES}
		   )

install( TARGETS  uxplay RUNTIME DESTINATION bin )
install( FILES uxplay.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 )
install( FILES README.md README.txt README.html LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR} ) 
install( FILES lib/llhttp/LICENSE-MIT DESTINATION ${CMAKE_INSTALL_DOCDIR}/llhttp ) 

# uninstall target
if(NOT TARGET uninstall)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

  add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()
