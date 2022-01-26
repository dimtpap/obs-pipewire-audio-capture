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

#define LPWA_TARGET_NAME "LPWA_TARGET_NAME"

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

struct pipewire_data {
	obs_source_t *context;

	enum pipewire_audio_capture_type capture_type;

	uint32_t pw_self_id;

	DARRAY(struct pipewire_node) nodes_arr;
	uint32_t pw_target_id;

	struct spa_hook *registry_listner;
	struct pw_stream *pw_stream;

	uint32_t frame_size;
	uint32_t sample_rate;
	enum audio_format format;
	enum speaker_layout speakers;
};

//Utilities
struct pipewire_node *get_node_by_name(const char *name,
				       DARRAY(struct pipewire_node) * nodes_arr)
{
	for (size_t i = 0; i < nodes_arr->num; i++) {
		struct pipewire_node *node = darray_item(
			sizeof(*nodes_arr->array), &nodes_arr->da, i);
		if (node && strcmp(node->name, name) == 0)
			return node;
	}
	return NULL;
}
struct pipewire_node *
get_node_and_idx_by_id(uint32_t id, DARRAY(struct pipewire_node) * nodes_arr,
		       size_t *idx)
{
	for (size_t i = 0; i < nodes_arr->num; i++) {
		struct pipewire_node *node = darray_item(
			sizeof(*nodes_arr->array), &nodes_arr->da, i);
		if (node && node->id == id) {
			*idx = i;
			return node;
		}
	}
	return NULL;
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
//

//PipeWire Stream stuff
static void on_stream_process(void *data)
{
	struct pipewire_data *lpwa = data;
	struct pw_buffer *b;
	struct spa_buffer *buf;

	b = pw_stream_dequeue_buffer(lpwa->pw_stream);

	if (!b)
		goto done;

	buf = b->buffer;

	void *d = buf->datas[0].data;
	if (d == NULL || buf->datas[0].type != SPA_DATA_MemPtr)
		goto queue;

	struct obs_source_audio out;
	out.data[0] = d;
	out.speakers = lpwa->speakers;
	out.samples_per_sec = lpwa->sample_rate;
	out.format = lpwa->format;
	out.frames = buf->datas[0].chunk->size / lpwa->frame_size;
	out.timestamp = get_sample_time(out.frames, out.samples_per_sec);

	obs_source_output_audio(lpwa->context, &out);

queue:
	pw_stream_queue_buffer(lpwa->pw_stream, b);

done:
	pipewire_continue();
}

static void on_stream_param_changed(void *data, uint32_t id,
				    const struct spa_pod *param)
{
	if (!param || id != SPA_PARAM_Format)
		goto done;

	struct pipewire_data *lpwa = data;

	struct spa_audio_info_raw audio_info;
	spa_format_audio_raw_parse(param, &audio_info);

	lpwa->sample_rate = audio_info.rate;
	lpwa->speakers = spa_to_obs_speakers(audio_info.channels);
	lpwa->format = spa_to_obs_audio_format(audio_info.format);
	lpwa->frame_size =
		calculate_frame_size(lpwa->format, audio_info.channels);

done:
	pipewire_continue();
}

const struct pw_stream_events stream_callbacks = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_stream_process,
	.param_changed = on_stream_param_changed,
};
//

//Properties
static obs_properties_t *pipewire_capture_properties(void *data)
{
	struct pipewire_data *lpwa = data;

	const char *capture_type;
	capture_type = obs_module_text("Device");
	if (lpwa->capture_type == PIPEWIRE_AUDIO_CAPTURE_APPLICATION)
		capture_type = obs_module_text("Application");

	obs_properties_t *p = obs_properties_create();
	obs_property_t *devices_list = obs_properties_add_list(
		p, LPWA_TARGET_NAME, capture_type, OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);

	if (lpwa->capture_type != PIPEWIRE_AUDIO_CAPTURE_APPLICATION)
		obs_property_list_add_string(devices_list, "Default", "ANY");

	for (size_t i = 0; i < lpwa->nodes_arr.num; i++) {
		struct pipewire_node *node = darray_item(
			sizeof(*lpwa->nodes_arr.array), &lpwa->nodes_arr.da, i);
		obs_property_list_add_string(devices_list, node->friendly_name,
					     node->name);
	}
	return p;
}

static void pipewire_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, LPWA_TARGET_NAME, "ANY");
}
//

static void pipewire_capture_update(void *data, obs_data_t *settings)
{
	struct pipewire_data *lpwa = data;

	if (!lpwa->nodes_arr.num)
		return;

	const char *new_node_name;
	uint32_t new_node_id = 0;

	new_node_name = obs_data_get_string(settings, LPWA_TARGET_NAME);

	if (!new_node_name)
		return;

	if (strcmp(new_node_name, "ANY") == 0) {
		new_node_id = PW_ID_ANY;
		lpwa->pw_target_id = 0;
	} else {
		struct pipewire_node *new_node =
			get_node_by_name(new_node_name, &lpwa->nodes_arr);

		if (!new_node)
			return;
		new_node_id = new_node->id;

		if (new_node_id == lpwa->pw_target_id)
			return;
		lpwa->pw_target_id = new_node_id;
	}

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
			SPA_POD_CHOICE_RANGE_Int(0, 1, 8),
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

static void pipewire_global_added(void *data, uint32_t id, uint32_t permissions,
				  const char *type, uint32_t version,
				  const struct spa_dict *props)
{
	UNUSED_PARAMETER(permissions);
	UNUSED_PARAMETER(version);
	if (!props || !id)
		goto done;

	struct pipewire_data *lpwa = data;

	if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
		if (id == lpwa->pw_self_id)
			goto done;

		const char *media_class =
			spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
		if (!media_class)
			goto done;

		if ((lpwa->capture_type == PIPEWIRE_AUDIO_CAPTURE_INPUT &&
		     strcmp(media_class, "Audio/Source") == 0) ||
		    (lpwa->capture_type == PIPEWIRE_AUDIO_CAPTURE_OUTPUT &&
		     strcmp(media_class, "Audio/Sink") == 0) ||
		    (lpwa->capture_type == PIPEWIRE_AUDIO_CAPTURE_APPLICATION &&
		     strcmp(media_class, "Stream/Output/Audio") == 0)) {

			const char *node_name =
				spa_dict_lookup(props, PW_KEY_NODE_NAME);
			if (!node_name)
				goto done;

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
			//name so we save the info of the nodes we care about in a darray
			struct pipewire_node *node =
				da_push_back_new(lpwa->nodes_arr);
			node->friendly_name = bstrdup(node_friendly_name);
			node->name = bstrdup(node_name);
			node->id = id;
			blog(LOG_DEBUG,
			     "[PipeWire Audio Capture] Node registered - %s - %s - %d",
			     node->friendly_name, node->name, node->id);

			//If we are disconnected, a node for the same target
			//(with the same name) might connect later, connect to that
			if (lpwa->pw_stream &&
			    pw_stream_get_state(lpwa->pw_stream, NULL) ==
				    PW_STREAM_STATE_UNCONNECTED) {
				obs_data_t *settings =
					obs_source_get_settings(lpwa->context);
				const char *target_name = obs_data_get_string(
					settings, LPWA_TARGET_NAME);

				if (target_name &&
				    strcmp(target_name, node->name) == 0) {
					pipewire_capture_update(lpwa, settings);
				}
				obs_data_release(settings);
			}
		}
	}

done:
	pipewire_continue();
}

static void pipewire_global_removed(void *data, uint32_t id)
{
	struct pipewire_data *lpwa = data;
	if (id == lpwa->pw_self_id)
		goto done;

	size_t idx = 0;

	struct pipewire_node *node =
		get_node_and_idx_by_id(id, &lpwa->nodes_arr, &idx);
	if (node) {
		blog(LOG_DEBUG,
		     "[PipeWire Audio Capture] Node removed - %s - %s - %d",
		     node->friendly_name, node->name, node->id);
		bfree((void *)node->friendly_name);
		bfree((void *)node->name);
		da_erase(lpwa->nodes_arr, idx);
	} else
		goto done;

	/*If the object we're connected to is lost, try to find one with the same name
	This is useful for application capture, applications disconnect and connect frequently
	and some times have multiple streams running with the same name
	(For example, Firefox runs a new stream for every tab)*/
	if (id == lpwa->pw_target_id) {
		pipewire_stream_disconnect(lpwa->pw_stream);
		obs_data_t *settings = obs_source_get_settings(lpwa->context);
		pipewire_capture_update(lpwa, settings);
		obs_data_release(settings);
	}

done:
	pipewire_continue();
}

const struct pw_registry_events registry_events_enum = {
	PW_VERSION_REGISTRY_EVENTS, .global = pipewire_global_added,
	.global_remove = pipewire_global_removed};

static void *
pipewire_capture_create(obs_data_t *settings, obs_source_t *source,
			enum pipewire_audio_capture_type capture_type)
{
	struct pipewire_data *lpwa = bzalloc(sizeof(*lpwa));
	lpwa->context = source;
	lpwa->capture_type = capture_type;
	da_init(lpwa->nodes_arr);

	pipewire_init();

	bool capture_sink = true;
	if (capture_type == PIPEWIRE_AUDIO_CAPTURE_INPUT)
		capture_sink = false;

	lpwa->pw_stream =
		pipewire_stream_new(capture_sink, &stream_callbacks, lpwa);
	lpwa->pw_self_id = pw_stream_get_node_id(lpwa->pw_stream);

	lpwa->registry_listner = bzalloc(sizeof(struct spa_hook));
	pipewire_add_registry_listener(true, lpwa->registry_listner,
				       &registry_events_enum, lpwa);

	pipewire_capture_update(lpwa, settings);

	return lpwa;
}

static void pipewire_capture_destroy(void *data)
{
	struct pipewire_data *lpwa = data;

	for (size_t i = 0; i < lpwa->nodes_arr.num; i++) {
		struct pipewire_node *node = darray_item(
			sizeof(*lpwa->nodes_arr.array), &lpwa->nodes_arr.da, i);
		bfree((void *)node->friendly_name);
		bfree((void *)node->name);
		da_erase(lpwa->nodes_arr, i);
	}

	da_free(lpwa->nodes_arr);

	if (lpwa->pw_stream)
		pipewire_stream_destroy(lpwa->pw_stream);

	spa_hook_remove(lpwa->registry_listner);
	bfree(lpwa->registry_listner);

	bfree(lpwa);

	pipewire_unref();
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