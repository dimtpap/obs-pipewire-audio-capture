/* pipewire-audio.h
 *
 * Copyright 2022 Dimitris Papaioannou <dimtpap@protonmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/** Stuff used by the PipeWire audio capture sources */

#pragma once

#include <obs-module.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>

/**
 * Common PipeWire components
 */
struct obs_pw_audio_instance {
	struct pw_thread_loop *thread_loop;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;
	int seq;

	struct pw_registry *registry;
	struct spa_hook registry_listener;
};

/**
 * Initialize a PipeWire instance
 * @return true on success, false on error
 */
bool obs_pw_audio_instance_init(struct obs_pw_audio_instance *pw);

/**
 * Destroy a PipeWire instance
 * @warning Call with the thread loop locked
 */
void obs_pw_audio_instance_destroy(struct obs_pw_audio_instance *pw);

/**
 * Trigger a PipeWire core sync
 */
void obs_pw_audio_instance_sync(struct obs_pw_audio_instance *pw);
/* ------------------------------------------------- */

/* PipeWire Stream wrapper */

/**
 * Audio metadata
 */
struct obs_pw_audio_info {
	uint32_t frame_size;
	uint32_t sample_rate;
	enum audio_format format;
	enum speaker_layout speakers;
};

/**
 * PipeWire stream wrapper that outputs to an OBS source
 */
struct obs_pw_audio_stream {
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct obs_pw_audio_info info;
	struct spa_io_position *pos;

	obs_source_t *output;
};

/**
 * Initialize a stream
 * @return true on success, false on error
 */
bool obs_pw_audio_stream_init(struct obs_pw_audio_stream *s,
			      struct obs_pw_audio_instance *pw,
			      struct pw_properties *props,
			      obs_source_t *output);

/**
 * Destroy a stream
 */
void obs_pw_audio_stream_destroy(struct obs_pw_audio_stream *s);

/**
 * Connect a stream with the default params
 * @return 0 on success, < 0 on error
 */
int obs_pw_audio_stream_connect(struct obs_pw_audio_stream *s,
				enum spa_direction direction,
				uint32_t target_id, enum pw_stream_flags flags,
				uint32_t channels);

/**
 * Default PipeWire stream properties
 */
struct pw_properties *obs_pw_audio_stream_properties(bool capture_sink);
/* ------------------------------------------------- */

/**
 * PipeWire metadata
 */
struct obs_pw_audio_default_node_metadata {
	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	struct spa_hook metadata_listener;

	bool wants_sink;

	void (*default_node_callback)(void *data, const char *name);
	void *data;
};

/**
 * Add listeners to the metadata
 * @return true on success, false on error
 */
bool obs_pw_audio_default_node_metadata_listen(
	struct obs_pw_audio_default_node_metadata *metadata,
	struct obs_pw_audio_instance *pw, uint32_t global_id, bool wants_sink,
	void (*default_node_callback)(void *data, const char *name),
	void *data);
/* ------------------------------------------------- */

/**
 * Generic proxy handler for PipeWire objects tracked in lists
 */
struct obs_pw_audio_proxied_object {
	void *data;

	void (*bound_callback)(void *data, uint32_t global_id);
	void (*destroy_callback)(void *data);

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;

	struct spa_list link;
};

/**
 * Initialize a proxied object
 */
void obs_pw_audio_proxied_object_init(
	struct obs_pw_audio_proxied_object *obj, struct pw_proxy *proxy,
	struct spa_list *list,
	void (*bound_callback)(void *data, uint32_t global_id),
	void (*destroy_callback)(void *data), void *data);

/* Sources */

void pipewire_audio_capture_load(void);
void pipewire_audio_capture_app_load(void);
/* ------------------------------------------------- */
