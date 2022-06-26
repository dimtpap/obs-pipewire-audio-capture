cmake_minimum_required(VERSION 3.10)
project(linux-pipewire-audio)

if(BUILD_STANDALONE OR ENABLE_PIPEWIRE)

	set(linux-pipewire-audio_SOURCES
			linux-pipewire-audio.c
			pipewire-audio.h
			pipewire-audio.c
			pipewire-audio-capture.c
			pipewire-audio-capture-app.c
	)

	set(linux-capture-audio_HEADERS
		pipewire-wrapper.h
	)

	add_library(linux-pipewire-audio MODULE ${linux-pipewire-audio_SOURCES})
endif()

if(BUILD_STANDALONE)
	list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

	find_package(LibObs REQUIRED)
	find_package(PipeWire REQUIRED)
else()
	option(ENABLE_PIPEWIRE "Enable PipeWire support" ON)
	if(ENABLE_PIPEWIRE)
		find_package(PipeWire QUIET)

		if(NOT PIPEWIRE_FOUND)
			message(FATAL_ERROR "PipeWire library not found! Please install PipeWire or set ENABLE_PIPEWIRE=OFF")
		endif()

		add_definitions(-DENABLE_PIPEWIRE)
	endif()
endif()

if(BUILD_STANDALONE OR ENABLE_PIPEWIRE)
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
endif()

if(BUILD_STANDALONE)
	set_target_properties(linux-pipewire-audio PROPERTIES PREFIX "")

	install(TARGETS linux-pipewire-audio LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/obs-plugins)
	install(DIRECTORY data/locale DESTINATION ${CMAKE_INSTALL_PREFIX}/share/obs/obs-plugins/linux-pipewire-audio)
elseif(ENABLE_PIPEWIRE)
	set_target_properties(linux-pipewire-audio PROPERTIES FOLDER "plugins")
	install_obs_plugin_with_data(linux-pipewire-audio data)
endif()