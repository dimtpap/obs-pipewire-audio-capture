cmake_minimum_required(VERSION 3.10)
project(linux-pipewire-audio)

include(GNUInstallDirs)

set(linux-pipewire-audio_SOURCES
			src/linux-pipewire-audio.c
			src/pipewire-audio.h
			src/pipewire-audio.c
			src/pipewire-audio-capture-device.c
			src/pipewire-audio-capture-app.c
)

add_library(linux-pipewire-audio MODULE ${linux-pipewire-audio_SOURCES})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

find_package(libobs REQUIRED)
find_package(PipeWire REQUIRED)

set(linux-pipewire-audio_INCLUDES
	${PIPEWIRE_INCLUDE_DIRS}
	${SPA_INCLUDE_DIRS}
)

add_definitions(
	${PIPEWIRE_DEFINITIONS}
)

set(linux-pipewire-audio_LIBRARIES
	OBS::libobs
	${PIPEWIRE_LIBRARIES}
)

target_link_libraries(linux-pipewire-audio ${linux-pipewire-audio_LIBRARIES})
target_compile_options(linux-pipewire-audio PRIVATE -Wall)

include_directories(SYSTEM
	${linux-pipewire-audio_INCLUDES}
)

set_target_properties(linux-pipewire-audio PROPERTIES PREFIX "")

option(LEGACY "Build as a legacy plugin for OBS < 33")

if(libobs_VERSION_MAJOR LESS 33 AND NOT LEGACY)
  message(WARNING "Detected OBS version < 33, but LEGACY is not set. The plugin will be built for OBS version >= 33.")
endif()

if(LEGACY)
  install(TARGETS linux-pipewire-audio LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-plugins)
  install(DIRECTORY data/locale DESTINATION ${CMAKE_INSTALL_DATADIR}/obs/obs-plugins/linux-pipewire-audio)
else()
  install(TARGETS linux-pipewire-audio LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-modules/plugins)
  install(DIRECTORY data/locale DESTINATION ${CMAKE_INSTALL_DATADIR}/obs/obs-modules/plugins/linux-pipewire-audio)
endif()

