/* pipewire-audio.c
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

#include "pipewire-audio.h"

#include <obs-module.h>
#include <util/platform.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/json.h>

/** Utilities */
bool json_object_find(const char *obj, const char *key, char *value, size_t len)
{
	/** From PipeWire's source */

	struct spa_json it[2];
	const char *v;
	char k[128];

	spa_json_init(&it[0], obj, strlen(obj));
	if (spa_json_enter_object(&it[0], &it[1]) <= 0) {
		return false;
	}

	while (spa_json_get_string(&it[1], k, sizeof(k)) > 0) {
		if (spa_streq(k, key)) {
			if (spa_json_get_string(&it[1], value, len) > 0) {
				return true;
			}
		} else if (spa_json_next(&it[1], &v) <= 0) {
			break;
		}
	}
	return false;
}
/* ------------------------------------------------- */

/** Common PipeWire components */
static void on_core_done_cb(void *data, uint32_t id, int seq)
{
	struct obs_pw_audio_instance *pw = data;

	if (id == PW_ID_CORE && pw->seq == seq) {
		pw_thread_loop_signal(pw->thread_loop, false);
	}
}

static void on_core_error_cb(void *data, uint32_t id, int seq, int res,
			     const char *message)
{
	struct obs_pw_audio_instance *pw = data;

	blog(LOG_ERROR, "[pipewire] Error id:%u seq:%d res:%d :%s", id, seq,
	     res, message);

	pw_thread_loop_signal(pw->thread_loop, false);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done_cb,
	.error = on_core_error_cb,
};

bool obs_pw_audio_instance_init(struct obs_pw_audio_instance *pw)
{
	pw->thread_loop = pw_thread_loop_new("PipeWire thread loop", NULL);
	pw->context = pw_context_new(pw_thread_loop_get_loop(pw->thread_loop),
				     NULL, 0);

	pw_thread_loop_lock(pw->thread_loop);

	if (pw_thread_loop_start(pw->thread_loop) < 0) {
		blog(LOG_WARNING,
		     "[pipewire] Error starting threaded mainloop");
		pw_thread_loop_unlock(pw->thread_loop);
		return false;
	}

	pw->core = pw_context_connect(pw->context, NULL, 0);
	if (!pw->core) {
		blog(LOG_WARNING, "[pipewire] Error creating PipeWire core");
		pw_thread_loop_unlock(pw->thread_loop);
		return false;
	}

	pw_core_add_listener(pw->core, &pw->core_listener, &core_events, pw);

	pw->registry = pw_core_get_registry(pw->core, PW_VERSION_REGISTRY, 0);
	if (!pw->registry) {
		pw_thread_loop_unlock(pw->thread_loop);
		return false;
	}

	pw_thread_loop_unlock(pw->thread_loop);
	return true;
}

void obs_pw_audio_instance_destroy(struct obs_pw_audio_instance *pw)
{
	if (pw->registry) {
		spa_hook_remove(&pw->registry_listener);
		spa_zero(pw->registry_listener);
		pw_proxy_destroy((struct pw_proxy *)pw->registry);
	}

	pw_thread_loop_unlock(pw->thread_loop);
	pw_thread_loop_stop(pw->thread_loop);

	if (pw->core) {
		spa_hook_remove(&pw->core_listener);
		spa_zero(pw->core_listener);
		pw_core_disconnect(pw->core);
	}

	if (pw->context) {
		pw_context_destroy(pw->context);
	}

	pw_thread_loop_destroy(pw->thread_loop);
}

void obs_pw_audio_instance_sync(struct obs_pw_audio_instance *pw)
{
	pw->seq = pw_core_sync(pw->core, PW_ID_CORE, pw->seq);
}
/* ------------------------------------------------- */

/* PipeWire stream wrapper */
static uint32_t obs_audio_format_sample_size(enum audio_format audio_format)
{
	switch (audio_format) {
	case AUDIO_FORMAT_U8BIT:
		return 1;
	case AUDIO_FORMAT_16BIT:
		return 2;
	case AUDIO_FORMAT_32BIT:
	case AUDIO_FORMAT_FLOAT:
		return 4;
	default:
		return 2;
	}
}

static inline uint64_t audio_frames_to_nanosecs(uint32_t sample_rate,
						uint32_t frames)
{
	return util_mul_div64(frames, SPA_NSEC_PER_SEC, sample_rate);
}

void obs_channels_to_spa_audio_position(uint32_t *position, uint32_t channels)
{
	switch (channels) {
	case 8:
		position[0] = SPA_AUDIO_CHANNEL_FL;
		position[1] = SPA_AUDIO_CHANNEL_FR;
		position[2] = SPA_AUDIO_CHANNEL_FC;
		position[3] = SPA_AUDIO_CHANNEL_LFE;
		position[4] = SPA_AUDIO_CHANNEL_RL;
		position[5] = SPA_AUDIO_CHANNEL_RR;
		position[6] = SPA_AUDIO_CHANNEL_SL;
		position[7] = SPA_AUDIO_CHANNEL_SR;
		break;
	case 6:
		position[0] = SPA_AUDIO_CHANNEL_FL;
		position[1] = SPA_AUDIO_CHANNEL_FR;
		position[2] = SPA_AUDIO_CHANNEL_FC;
		position[3] = SPA_AUDIO_CHANNEL_LFE;
		position[4] = SPA_AUDIO_CHANNEL_RL;
		position[5] = SPA_AUDIO_CHANNEL_RR;
		break;
	case 5:
		position[0] = SPA_AUDIO_CHANNEL_FL;
		position[1] = SPA_AUDIO_CHANNEL_FR;
		position[2] = SPA_AUDIO_CHANNEL_FC;
		position[3] = SPA_AUDIO_CHANNEL_LFE;
		position[4] = SPA_AUDIO_CHANNEL_RC;
		break;
	case 4:
		position[0] = SPA_AUDIO_CHANNEL_FL;
		position[1] = SPA_AUDIO_CHANNEL_FR;
		position[2] = SPA_AUDIO_CHANNEL_FC;
		position[3] = SPA_AUDIO_CHANNEL_RC;
		break;
	case 3:
		position[0] = SPA_AUDIO_CHANNEL_FL;
		position[1] = SPA_AUDIO_CHANNEL_FR;
		position[2] = SPA_AUDIO_CHANNEL_LFE;
		break;
	case 2:
		position[0] = SPA_AUDIO_CHANNEL_FL;
		position[1] = SPA_AUDIO_CHANNEL_FR;
		break;
	case 1:
		position[0] = SPA_AUDIO_CHANNEL_MONO;
		break;
	default:
		for (size_t i = 0; i < channels; i++) {
			position[i] = SPA_AUDIO_CHANNEL_UNKNOWN;
		}
		break;
	}
}

enum audio_format spa_to_obs_audio_format(enum spa_audio_format format)
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

enum speaker_layout spa_to_obs_speakers(uint32_t channels)
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

bool spa_to_obs_pw_audio_info(struct obs_pw_audio_info *info,
			      const struct spa_pod *param)
{
	struct spa_audio_info_raw audio_info;

	if (spa_format_audio_raw_parse(param, &audio_info) < 0) {
		return false;
	}

	info->sample_rate = audio_info.rate;
	info->speakers = spa_to_obs_speakers(audio_info.channels);
	info->format = spa_to_obs_audio_format(audio_info.format);
	info->frame_size = obs_audio_format_sample_size(info->format) *
			   audio_info.channels;

	return true;
}

static void on_process_cb(void *data)
{
	uint64_t now = os_gettime_ns();

	struct obs_pw_audio_stream *s = data;

	struct pw_buffer *b = pw_stream_dequeue_buffer(s->stream);

	if (!b) {
		return;
	}

	struct spa_buffer *buf = b->buffer;

	void *d = buf->datas[0].data;
	if (!d || !s->info.frame_size || !s->info.sample_rate ||
	    buf->datas[0].type != SPA_DATA_MemPtr) {
		goto queue;
	}

	struct obs_source_audio out;
	out.data[0] = d;
	out.frames = buf->datas[0].chunk->size / s->info.frame_size;
	out.speakers = s->info.speakers;
	out.format = s->info.format;
	out.samples_per_sec = s->info.sample_rate;

	if (s->pos && (s->info.sample_rate * s->pos->clock.rate_diff)) {
		/** Taken from PipeWire's implementation of JACK's jack_get_cycle_times
		  * (https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/0.3.52/pipewire-jack/src/pipewire-jack.c#L5639)
	   	  * which is used in the linux-jack plugin to correctly set the timestamp
		  * (https://github.com/obsproject/obs-studio/blob/27.2.4/plugins/linux-jack/jack-wrapper.c#L87) */

		float period_usecs =
			s->pos->clock.duration * (float)SPA_USEC_PER_SEC /
			(s->info.sample_rate * s->pos->clock.rate_diff);

		out.timestamp = now - (uint64_t)(period_usecs * 1000);
	} else {
		out.timestamp = now - audio_frames_to_nanosecs(
					      out.frames, s->info.sample_rate);
	}

	obs_source_output_audio(s->output, &out);

queue:
	pw_stream_queue_buffer(s->stream, b);
}

static void on_state_changed_cb(void *data, enum pw_stream_state old,
				enum pw_stream_state state, const char *error)
{
	UNUSED_PARAMETER(old);

	struct obs_pw_audio_stream *s = data;

	blog(LOG_DEBUG, "[pipewire] Stream %p state: \"%s\" (error: %s)",
	     s->stream, pw_stream_state_as_string(state),
	     error ? error : "none");
}

static void on_param_changed_cb(void *data, uint32_t id,
				const struct spa_pod *param)
{
	if (!param || id != SPA_PARAM_Format) {
		return;
	}

	struct obs_pw_audio_stream *s = data;

	if (!spa_to_obs_pw_audio_info(&s->info, param)) {
		blog(LOG_WARNING,
		     "[pipewire] Stream %p failed to parse audio format info",
		     s->stream);
		s->info.sample_rate = 0;
		s->info.speakers = SPEAKERS_UNKNOWN;
		s->info.format = AUDIO_FORMAT_UNKNOWN;
		s->info.frame_size = 0;
	} else {
		blog(LOG_INFO,
		     "[pipewire] %p Got format: rate %u - channels %u - format %u - frame size %u",
		     s->stream, s->info.sample_rate, s->info.speakers,
		     s->info.format, s->info.frame_size);
	}

	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	const struct spa_pod *params[1];
	params[0] = spa_pod_builder_add_object(
		&b, SPA_TYPE_OBJECT_ParamIO, SPA_PARAM_IO, SPA_PARAM_IO_id,
		SPA_POD_Id(SPA_IO_Position), SPA_PARAM_IO_size,
		SPA_POD_Int(sizeof(struct spa_io_position)));

	pw_stream_update_params(s->stream, params, 1);
}

static void on_io_changed_cb(void *data, uint32_t id, void *area, uint32_t size)
{
	UNUSED_PARAMETER(size);

	struct obs_pw_audio_stream *s = data;

	if (id == SPA_IO_Position) {
		s->pos = area;
	}
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process_cb,
	.state_changed = on_state_changed_cb,
	.param_changed = on_param_changed_cb,
	.io_changed = on_io_changed_cb,
};

bool obs_pw_audio_stream_init(struct obs_pw_audio_stream *s,
			      struct obs_pw_audio_instance *pw,
			      struct pw_properties *props, obs_source_t *output)
{
	s->output = output;
	s->stream = pw_stream_new(pw->core, "OBS Studio", props);

	if (!s->stream) {
		return false;
	}

	pw_stream_add_listener(s->stream, &s->stream_listener, &stream_events,
			       s);
	return true;
}

void obs_pw_audio_stream_destroy(struct obs_pw_audio_stream *s)
{
	if (s->stream) {
		spa_hook_remove(&s->stream_listener);
		pw_stream_disconnect(s->stream);
		pw_stream_destroy(s->stream);

		memset(s, 0, sizeof(struct obs_pw_audio_stream));
	}
}

int obs_pw_audio_stream_connect(struct obs_pw_audio_stream *s,
				enum spa_direction direction,
				uint32_t target_id, enum pw_stream_flags flags,
				uint32_t audio_channels)
{
	uint32_t pos[8];
	obs_channels_to_spa_audio_position(pos, audio_channels);

	uint8_t buffer[2048];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];

	params[0] = spa_pod_builder_add_object(
		&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_AUDIO_channels, SPA_POD_Int(audio_channels),
		SPA_FORMAT_AUDIO_position,
		SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Id, audio_channels,
			      pos),
		SPA_FORMAT_AUDIO_format,
		SPA_POD_CHOICE_ENUM_Id(
			4, SPA_AUDIO_FORMAT_U8, SPA_AUDIO_FORMAT_S16_LE,
			SPA_AUDIO_FORMAT_S32_LE, SPA_AUDIO_FORMAT_F32_LE));

	return pw_stream_connect(s->stream, direction, target_id, flags, params,
				 1);
}

struct pw_properties *obs_pw_audio_stream_properties(bool capture_sink)
{
	return pw_properties_new(
		PW_KEY_NODE_NAME, "OBS Studio", PW_KEY_NODE_DESCRIPTION,
		"OBS Audio Capture", PW_KEY_APP_NAME, "OBS Studio",
		PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_MEDIA_ROLE, "Production", PW_KEY_STREAM_CAPTURE_SINK,
		capture_sink ? "true" : "false", NULL);
}
/* ------------------------------------------------- */

/* PipeWire metadata */
static void on_metadata_proxy_removed_cb(void *data)
{
	struct obs_pw_audio_metadata *metadata = data;
	pw_proxy_destroy(metadata->proxy);
}

static void on_metadata_proxy_destroy_cb(void *data)
{
	struct obs_pw_audio_metadata *metadata = data;

	spa_hook_remove(&metadata->metadata_listener);
	spa_hook_remove(&metadata->proxy_listener);
	spa_zero(metadata->metadata_listener);
	spa_zero(metadata->proxy_listener);

	metadata->proxy = NULL;
}

static const struct pw_proxy_events metadata_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = on_metadata_proxy_removed_cb,
	.destroy = on_metadata_proxy_destroy_cb,
};

bool obs_pw_audio_metadata_listen(
	struct obs_pw_audio_metadata *metadata,
	struct obs_pw_audio_instance *pw, uint32_t global_id,
	const struct pw_metadata_events *metadata_events, void *data)
{
	if (metadata->proxy) {
		pw_proxy_destroy(metadata->proxy);
	}

	struct pw_proxy *metadata_proxy = pw_registry_bind(
		pw->registry, global_id, PW_TYPE_INTERFACE_Metadata,
		PW_VERSION_METADATA, 0);
	if (!metadata_proxy) {
		return false;
	}

	metadata->proxy = metadata_proxy;

	pw_proxy_add_object_listener(metadata->proxy,
				     &metadata->metadata_listener,
				     metadata_events, data);
	pw_proxy_add_listener(metadata->proxy, &metadata->proxy_listener,
			      &metadata_proxy_events, metadata);

	return true;
}
/* ------------------------------------------------- */

/** Proxied objects */
static void on_proxy_bound_cb(void *data, uint32_t global_id)
{
	struct obs_pw_audio_proxied_object *obj = data;
	if (obj->bound_callback) {
		obj->bound_callback(obj->data, global_id);
	}
}

static void on_proxy_removed_cb(void *data)
{
	struct obs_pw_audio_proxied_object *obj = data;
	pw_proxy_destroy(obj->proxy);
}

static void on_proxy_destroy_cb(void *data)
{
	struct obs_pw_audio_proxied_object *obj = data;
	spa_hook_remove(&obj->proxy_listener);

	spa_list_remove(&obj->link);

	if (obj->destroy_callback) {
		obj->destroy_callback(obj->data);
	}

	bfree(obj->data);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.bound = on_proxy_bound_cb,
	.removed = on_proxy_removed_cb,
	.destroy = on_proxy_destroy_cb,
};

void obs_pw_audio_proxied_object_init(
	struct obs_pw_audio_proxied_object *obj, struct pw_proxy *proxy,
	struct spa_list *list,
	void (*bound_callback)(void *data, uint32_t global_id),
	void (*destroy_callback)(void *data), void *data)
{
	obj->proxy = proxy;
	obj->bound_callback = bound_callback;
	obj->destroy_callback = destroy_callback;
	obj->data = data;

	spa_list_append(list, &obj->link);

	spa_zero(obj->proxy_listener);
	pw_proxy_add_listener(obj->proxy, &obj->proxy_listener, &proxy_events,
			      obj);
}
/* ------------------------------------------------- */
