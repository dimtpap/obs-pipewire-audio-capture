/*
Copyright (C) 2021-2022 by Dimitris Papaioannou <jimpap31@outlook.com.gr>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <obs-module.h>

#include <pipewire/pipewire.h>

#include "pipewire-audio.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("linux-pipewire-audio", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "PipeWire input, output and application audio capture";
}

bool obs_module_load(void)
{
	pipewire_audio_capture_load();
	pipewire_audio_capture_app_load();
	return true;
}