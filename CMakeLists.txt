cmake_minimum_required(VERSION 3.10)
project(linux-pipewire-audio)

set(linux-pipewire-audio_SOURCES
			linux-pipewire-audio.c
			pipewire-audio.h
			pipewire-audio.c
			pipewire-audio-capture.c
			pipewire-audio-capture-app.c
)

add_library(linux-pipewire-audio MODULE ${linux-pipewire-audio_SOURCES})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

find_package(LibObs REQUIRED)
find_package(PipeWire REQUIRED)

set(linux-pipewire-audio_INCLUDES
	${PIPEWIRE_INCLUDE_DIRS}
	${SPA_INCLUDE_DIRS}
)

add_definitions(
	${PIPEWIRE_DEFINITIONS}
)

set(linux-pipewire-audio_LIBRARIES
	libobs
	${PIPEWIRE_LIBRARIES}
)

target_link_libraries(linux-pipewire-audio ${linux-pipewire-audio_LIBRARIES})

include_directories(SYSTEM
	${linux-pipewire-audio_INCLUDES}
)

set_target_properties(linux-pipewire-audio PROPERTIES PREFIX "")

install(TARGETS linux-pipewire-audio LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/obs-plugins)
install(DIRECTORY data/locale DESTINATION ${CMAKE_INSTALL_PREFIX}/share/obs/obs-plugins/linux-pipewire-audio)