/*
Copyright (C) 2021 by Dimitris Papaioannou <jimpap31@outlook.com.gr>

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
#include <util/util_uint64.h>
#include <util/platform.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/type-info.h>

#include "pipewire-wrapper.h"

enum pipewire_audio_capture_type {
	PIPEWIRE_AUDIO_CAPTURE_INPUT,
	PIPEWIRE_AUDIO_CAPTURE_OUTPUT,
	PIPEWIRE_AUDIO_CAPTURE_APPLICATION,
};

struct pipewire_node {
	const char *friendly_name;
	const char *name;
	uint32_t id;
};

uint32_t get_node_id_by_name(const char *name, struct pipewire_node *nodes_arr,
			     uint32_t n_nodes)
{
	for (size_t i = 0; i < n_nodes; i++) {
		if (strcmp(nodes_arr[i].name, name) == 0) {
			return nodes_arr[i].id;
		}
	}
	return 0;
}

struct pipewire_data {
	obs_source_t *context;

	enum pipewire_audio_capture_type capture_type;
	const char *capture_prop_desc;

	uint32_t pw_self_id;

	char *pw_target_name;
	uint32_t pw_target_id;

	uint32_t n_nodes;
	struct pipewire_node *nodes_arr;

	struct spa_hook stream_listener;
	struct pw_stream *pw_stream;

	uint32_t frame_size;
	uint32_t sample_rate;
	enum audio_format format;
	enum speaker_layout speakers;
};

const char *pipewire_input_capture_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("PipeWireInput");
}
const char *pipewire_output_capture_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("PipeWireOutput");
}
const char *pipewire_application_capture_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("PipeWireApplication");
}

static enum audio_format spa_to_obs_audio_format(enum spa_audio_format format)
{
	switch (format) {
	case SPA_AUDIO_FORMAT_U8:
		return AUDIO_FORMAT_U8BIT;
	case SPA_AUDIO_FORMAT_S16_LE:
		return AUDIO_FORMAT_16BIT;
	case SPA_AUDIO_FORMAT_S32_LE:
		return AUDIO_FORMAT_32BIT;
	case SPA_AUDIO_FORMAT_F32_LE:
		return AUDIO_FORMAT_FLOAT;
	default:
		return AUDIO_FORMAT_UNKNOWN;
	}
}

static enum speaker_layout spa_to_obs_speakers(uint32_t channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return SPEAKERS_UNKNOWN;
	}
}

static uint32_t obs_audio_format_bits(enum audio_format audio_format)
{
	switch (audio_format) {
	case AUDIO_FORMAT_U8BIT:
		return 8;
	case AUDIO_FORMAT_16BIT:
		return 16;
	case AUDIO_FORMAT_32BIT:
	case AUDIO_FORMAT_FLOAT:
		return 32;
	default:
		return 16;
	}
}

static inline uint32_t calculate_frame_size(enum audio_format format,
					    uint32_t channels)
{
	return obs_audio_format_bits(format) * channels / 8;
}
static inline uint64_t get_sample_time(uint32_t frames, uint32_t sample_rate)
{
	return os_gettime_ns() -
	       util_mul_div64(frames, SPA_NSEC_PER_SEC, sample_rate);
}

static void on_stream_process(void *data)
{
	struct pipewire_data *lpwa = data;
	struct pw_buffer *b;
	struct spa_buffer *buf;

	b = pw_stream_dequeue_buffer(lpwa->pw_stream);

	if (!b) {
		return;
	}

	buf = b->buffer;

	void *d = buf->datas[0].data;
	if (d == NULL || buf->datas[0].type != SPA_DATA_MemPtr)
		return;

	struct obs_source_audio out;
	out.data[0] = d;
	out.speakers = lpwa->speakers;
	out.samples_per_sec = lpwa->sample_rate;
	out.format = lpwa->format;
	out.frames = buf->datas[0].chunk->size / lpwa->frame_size;
	out.timestamp = get_sample_time(out.frames, out.samples_per_sec);

	obs_source_output_audio(lpwa->context, &out);

	pw_stream_queue_buffer(lpwa->pw_stream, b);
	pipewire_continue();
}

static void on_stream_param_changed(void *data, uint32_t id,
				    const struct spa_pod *param)
{
	if (!param || id != SPA_PARAM_Format) {
		pipewire_continue();
		return;
	}

	struct pipewire_data *lpwa = data;

	struct spa_audio_info_raw audio_info;
	int res = spa_format_audio_raw_parse(param, &audio_info);

	lpwa->sample_rate = audio_info.rate;
	lpwa->speakers = spa_to_obs_speakers(audio_info.channels);
	lpwa->format = spa_to_obs_audio_format(audio_info.format);
	lpwa->frame_size =
		calculate_frame_size(lpwa->format, audio_info.channels);
}

const struct pw_stream_events stream_callbacks = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_stream_process,
	.param_changed = on_stream_param_changed,
};

static void registry_event_enum_global(void *data, uint32_t id,
				       uint32_t permissions, const char *type,
				       uint32_t version,
				       const struct spa_dict *props)
{
	UNUSED_PARAMETER(permissions);
	UNUSED_PARAMETER(version);
	if (!props || !id) {
		pipewire_continue();
		return;
	}

	struct pipewire_data *lpwa = data;

	if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
		if (lpwa->pw_self_id) {
			if (id == lpwa->pw_self_id) {
				pipewire_continue();
				return;
			}
		}

		const char *media_class =
			spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
		if (!media_class) {
			pipewire_continue();
			return;
		}

		if ((lpwa->capture_type == PIPEWIRE_AUDIO_CAPTURE_INPUT &&
		     strcmp(media_class, "Audio/Source") == 0) ||
		    (lpwa->capture_type == PIPEWIRE_AUDIO_CAPTURE_OUTPUT &&
		     strcmp(media_class, "Audio/Sink") == 0) ||
		    (lpwa->capture_type == PIPEWIRE_AUDIO_CAPTURE_APPLICATION &&
		     strcmp(media_class, "Stream/Output/Audio") == 0)) {

			const char *node_name =
				spa_dict_lookup(props, PW_KEY_NODE_NAME);
			if (!node_name) {
				pipewire_continue();
				return;
			}

			const char *node_friendly_name;
			if (lpwa->capture_type ==
			    PIPEWIRE_AUDIO_CAPTURE_APPLICATION) {
				node_friendly_name =
					spa_dict_lookup(props, PW_KEY_APP_NAME);
			} else {
				node_friendly_name = spa_dict_lookup(
					props, PW_KEY_NODE_DESCRIPTION);
			}

			if (!node_friendly_name)
				node_friendly_name = node_name;

			//PipeWire does not provide an easy way to get info about a node using its
			//name so we save the info of the nodes we care about in an array

			struct pipewire_node node = {
				.name = node_name,
				.friendly_name = node_friendly_name,
				.id = id,
			};

			lpwa->nodes_arr =
				brealloc(lpwa->nodes_arr,
					 sizeof(node) * (lpwa->n_nodes + 1));
			lpwa->nodes_arr[lpwa->n_nodes] = node;
			lpwa->n_nodes += 1;
		}
	}

	pipewire_continue();
}

const struct pw_registry_events registry_events_enum = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_enum_global,
};

static obs_properties_t *pipewire_capture_properties(void *data)
{
	struct pipewire_data *lpwa = data;

	obs_properties_t *p = obs_properties_create();
	obs_property_t *devices_list = obs_properties_add_list(
		p, "PW_CAPTURE_TARGET_NAME", lpwa->capture_prop_desc,
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	if (lpwa->capture_type != PIPEWIRE_AUDIO_CAPTURE_APPLICATION) {
		obs_property_list_add_string(devices_list, "Default", "ANY");
	}

	lpwa->pw_self_id = pw_stream_get_node_id(lpwa->pw_stream);

	if (lpwa->n_nodes) {
		bfree(lpwa->nodes_arr);
		lpwa->n_nodes = 0;
		lpwa->nodes_arr = bmalloc(sizeof(struct pipewire_node));
	}
	pipewire_enum_objects(&registry_events_enum, lpwa);
	for (size_t i = 0; i < lpwa->n_nodes; i++) {
		obs_property_list_add_string(devices_list,
					     lpwa->nodes_arr[i].friendly_name,
					     lpwa->nodes_arr[i].name);
	}

	return p;
}

static void pipewire_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "PW_CAPTURE_TARGET_NAME", "ANY");
}

static void pipewire_capture_update(void *data, obs_data_t *settings)
{
	struct pipewire_data *lpwa = data;

	if (!lpwa->n_nodes)
		return;

	const char *new_node_name;
	uint32_t new_node_id = 0;

	new_node_name = obs_data_get_string(settings, "PW_CAPTURE_TARGET_NAME");

	if (!new_node_name)
		return;

	lpwa->pw_target_name = new_node_name;

	if (strcmp(new_node_name, "ANY") == 0) {
		new_node_id = PW_ID_ANY;
	} else {
		new_node_id = get_node_id_by_name(
			new_node_name, lpwa->nodes_arr, lpwa->n_nodes);
	}

	if (!new_node_id || lpwa->pw_target_id == new_node_id)
		return;

	lpwa->pw_target_id = new_node_id;

	if (lpwa->pw_stream) {
		uint8_t buffer[1024];
		struct spa_pod_builder b =
			SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

		const struct spa_pod *params[1];
		params[0] = spa_pod_builder_add_object(
			&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,
			SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_AUDIO_channels,
			SPA_POD_CHOICE_RANGE_Int(2, 1, 8),
			SPA_FORMAT_AUDIO_format,
			SPA_POD_CHOICE_ENUM_Id(5, SPA_AUDIO_FORMAT_UNKNOWN,
					       SPA_AUDIO_FORMAT_U8,
					       SPA_AUDIO_FORMAT_S16_LE,
					       SPA_AUDIO_FORMAT_S32_LE,
					       SPA_AUDIO_FORMAT_F32_LE));

		pipewire_stream_disconnect(lpwa->pw_stream);
		pipewire_stream_connect(lpwa->pw_stream, params, new_node_id);
	}
}

static void *
pipewire_capture_create(obs_data_t *settings, obs_source_t *source,
			enum pipewire_audio_capture_type capture_type)
{
	struct pipewire_data *lpwa = bzalloc(sizeof(*lpwa));
	lpwa->context = source;

	lpwa->capture_type = capture_type;
	lpwa->capture_prop_desc = obs_module_text("Device");
	if (capture_type == PIPEWIRE_AUDIO_CAPTURE_APPLICATION)
		lpwa->capture_prop_desc = obs_module_text("Application");

	pipewire_init();

	char *sink_capture_str = "true";
	if (capture_type == PIPEWIRE_AUDIO_CAPTURE_INPUT)
		sink_capture_str = "false";

	struct pw_properties *stream_props = pw_properties_new(
		PW_KEY_APP_NAME, "OBS Studio", PW_KEY_APP_ICON_NAME, "obs",
		PW_KEY_MEDIA_ROLE, "Production", PW_KEY_MEDIA_TYPE, "Audio",
		PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_STREAM_CAPTURE_SINK,
		sink_capture_str, NULL);

	spa_zero(lpwa->stream_listener);

	lpwa->pw_stream = pipewire_stream_new(
		stream_props, &lpwa->stream_listener, &stream_callbacks, lpwa);
	lpwa->pw_self_id = 0;

	lpwa->n_nodes = 0;
	lpwa->nodes_arr = bmalloc(sizeof(struct pipewire_node));
	pipewire_enum_objects(&registry_events_enum, lpwa);

	pipewire_capture_update(lpwa, settings);

	return lpwa;
}

static void *pipewire_input_capture_create(obs_data_t *settings,
					   obs_source_t *source)
{
	return pipewire_capture_create(settings, source,
				       PIPEWIRE_AUDIO_CAPTURE_INPUT);
}
static void *pipewire_output_capture_create(obs_data_t *settings,
					    obs_source_t *source)
{
	return pipewire_capture_create(settings, source,
				       PIPEWIRE_AUDIO_CAPTURE_OUTPUT);
}
static void *pipewire_application_capture_create(obs_data_t *settings,
						 obs_source_t *source)
{
	return pipewire_capture_create(settings, source,
				       PIPEWIRE_AUDIO_CAPTURE_APPLICATION);
}

static void pipewire_capture_destroy(void *data)
{
	struct pipewire_data *lpwa = data;

	spa_hook_remove(&lpwa->stream_listener);

	if (lpwa->pw_stream)
		pipewire_stream_destroy(lpwa->pw_stream);

	pipewire_unref();

	if (lpwa->n_nodes) {
		bfree(lpwa->nodes_arr);
	}
	bfree(lpwa);
}

struct obs_source_info pipewire_audio_input_capture = {
	.id = "pipewire_audio_input_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = pipewire_input_capture_name,
	.create = pipewire_input_capture_create,
	.get_properties = pipewire_capture_properties,
	.get_defaults = pipewire_capture_defaults,
	.update = pipewire_capture_update,
	.destroy = pipewire_capture_destroy,
	.icon_type = OBS_ICON_TYPE_AUDIO_INPUT,
};
struct obs_source_info pipewire_audio_output_capture = {
	.id = "pipewire_audio_output_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_DO_NOT_SELF_MONITOR,
	.get_name = pipewire_output_capture_name,
	.create = pipewire_output_capture_create,
	.get_properties = pipewire_capture_properties,
	.get_defaults = pipewire_capture_defaults,
	.update = pipewire_capture_update,
	.destroy = pipewire_capture_destroy,
	.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT,
};
struct obs_source_info pipewire_audio_application_capture = {
	.id = "pipewire_audio_application_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = pipewire_application_capture_name,
	.create = pipewire_application_capture_create,
	.get_properties = pipewire_capture_properties,
	.update = pipewire_capture_update,
	.destroy = pipewire_capture_destroy,
	.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT,
};