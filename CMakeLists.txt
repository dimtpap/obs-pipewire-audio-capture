project(linux-pipewire-audio)

option(ENABLE_PIPEWIRE "Enable PipeWire support" ON)
if(ENABLE_PIPEWIRE)
	find_package(PipeWire QUIET)

	if(NOT PIPEWIRE_FOUND)
		message(FATAL_ERROR "PipeWire library not found! Please install PipeWire or set ENABLE_PIPEWIRE=OFF")
	endif()

	add_definitions(-DENABLE_PIPEWIRE)

	set(linux-pipewire-audio_INCLUDES
    ${PIPEWIRE_INCLUDE_DIRS}
  )
  add_definitions(
    ${PIPEWIRE_DEFINITIONS}
  )

	set(linux-pipewire-audio_SOURCES
    linux-pipewire-audio.c
    pipewire-input.c
    pipewire-wrapper.c
  )
	set(linux-capture-audio_HEADERS
		pipewire-wrapper.h
	)
	set(linux-pipewire-audio_LIBRARIES
    libobs
  	${PIPEWIRE_LIBRARIES}
  ) 
endif()

include_directories(SYSTEM
  ${linux-pipewire-audio_INCLUDES}
)

add_library(linux-pipewire-audio MODULE ${linux-pipewire-audio_SOURCES})
target_link_libraries(linux-pipewire-audio ${linux-pipewire-audio_LIBRARIES})
set_target_properties(linux-pipewire-audio PROPERTIES FOLDER "plugins")

install_obs_plugin_with_data(linux-pipewire-audio data)