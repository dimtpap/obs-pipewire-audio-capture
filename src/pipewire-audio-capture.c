/* pipewire-audio-capture.c
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

#include <util/dstr.h>

/** Source for capturing device audio using PipeWire */
enum obs_pw_audio_capture_type {
	PIPEWIRE_AUDIO_CAPTURE_INPUT,
	PIPEWIRE_AUDIO_CAPTURE_OUTPUT,
};

struct target_node {
	const char *friendly_name;
	const char *name;
	uint32_t id;
	uint32_t channels;

	struct spa_hook node_listener;

	struct obs_pw_audio_capture *pwac;

	struct obs_pw_audio_proxied_object obj;
};

struct obs_pw_audio_capture {
	obs_source_t *source;

	enum obs_pw_audio_capture_type capture_type;

	struct obs_pw_audio_instance pw;

	struct obs_pw_audio_stream audio;

	struct {
		struct obs_pw_audio_default_node_metadata metadata;
		bool autoconnect;
		uint32_t node_id;
		struct dstr name;
	} default_info;

	struct spa_list targets;

	struct dstr target_name;
	uint32_t connected_id;
};

static void start_streaming(struct obs_pw_audio_capture *pwac, struct target_node *node)
{
	if (!pwac->audio.stream || !node || !node->channels) {
		return;
	}

	dstr_copy(&pwac->target_name, node->name);

	if (node->id == pwac->connected_id &&
		pw_stream_get_state(pwac->audio.stream, NULL) != PW_STREAM_STATE_UNCONNECTED) {
		/** Already connected to this node */
		return;
	}

	if (pw_stream_get_state(pwac->audio.stream, NULL) != PW_STREAM_STATE_UNCONNECTED) {
		pw_stream_disconnect(pwac->audio.stream);
	}

	if (obs_pw_audio_stream_connect(&pwac->audio, PW_DIRECTION_INPUT, node->id,
									PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, node->channels) == 0) {
		pwac->connected_id = node->id;
		blog(LOG_INFO, "[pipewire] %p streaming from %u", pwac->audio.stream, node->id);
	} else {
		pwac->connected_id = SPA_ID_INVALID;
		blog(LOG_WARNING, "[pipewire] Error connecting stream %p", pwac->audio.stream);
	}

	pw_stream_set_active(pwac->audio.stream, obs_source_active(pwac->source));
}

struct target_node *get_node_by_name(struct obs_pw_audio_capture *pwac, const char *name)
{
	struct target_node *n;
	spa_list_for_each(n, &pwac->targets, obj.link)
	{
		if (strcmp(n->name, name) == 0) {
			return n;
		}
	}
	return NULL;
}

struct target_node *get_node_by_id(struct obs_pw_audio_capture *pwac, uint32_t id)
{
	struct target_node *n;
	spa_list_for_each(n, &pwac->targets, obj.link)
	{
		if (n->id == id) {
			return n;
		}
	}
	return NULL;
}

/* Target node */
static void on_node_info_cb(void *data, const struct pw_node_info *info)
{
	if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) == 0 || !info->props || !info->props->n_items) {
		return;
	}

	const char *channels = spa_dict_lookup(info->props, PW_KEY_AUDIO_CHANNELS);
	if (!channels) {
		return;
	}

	uint32_t c = atoi(channels);

	struct target_node *n = data;
	if (n->channels == c) {
		return;
	}
	n->channels = c;

	struct obs_pw_audio_capture *pwac = n->pwac;

	/** If this is the default device and the stream is not already connected to it
	  * or the stream is unconnected and this node has the desired target name */
	if ((pwac->default_info.autoconnect && pwac->connected_id != n->id && !dstr_is_empty(&pwac->default_info.name) &&
		 dstr_cmp(&pwac->default_info.name, n->name) == 0) ||
		(pwac->audio.stream && pw_stream_get_state(pwac->audio.stream, NULL) == PW_STREAM_STATE_UNCONNECTED &&
		 !dstr_is_empty(&pwac->target_name) && dstr_cmp(&pwac->target_name, n->name) == 0)) {
		start_streaming(pwac, n);
	}
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = on_node_info_cb,
};

static void node_destroy_cb(void *data)
{
	struct target_node *n = data;

	spa_hook_remove(&n->node_listener);

	bfree((void *)n->friendly_name);
	bfree((void *)n->name);
}

static void register_target_node(struct obs_pw_audio_capture *pwac, const char *friendly_name, const char *name,
								 uint32_t global_id)
{
	struct pw_proxy *node_proxy =
		pw_registry_bind(pwac->pw.registry, global_id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
	if (!node_proxy) {
		return;
	}

	struct target_node *n = bmalloc(sizeof(struct target_node));
	n->friendly_name = bstrdup(friendly_name);
	n->name = bstrdup(name);
	n->id = global_id;
	n->channels = 0;
	n->pwac = pwac;

	obs_pw_audio_proxied_object_init(&n->obj, node_proxy, &pwac->targets, NULL, node_destroy_cb, n);

	spa_zero(n->node_listener);
	pw_proxy_add_object_listener(n->obj.proxy, &n->node_listener, &node_events, n);
}

/* Default device metadata */
static void default_node_cb(void *data, const char *name)
{
	struct obs_pw_audio_capture *pwac = data;

	blog(LOG_DEBUG, "[pipewire] New default device %s", name);

	dstr_copy(&pwac->default_info.name, name);

	struct target_node *n = get_node_by_name(pwac, name);
	if (n) {
		pwac->default_info.node_id = n->id;
		if (pwac->default_info.autoconnect) {
			start_streaming(pwac, n);
		}
	}
}
/* ------------------------------------------------- */

/* Registry */
static void on_global_cb(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version,
						 const struct spa_dict *props)
{
	UNUSED_PARAMETER(permissions);
	UNUSED_PARAMETER(version);

	struct obs_pw_audio_capture *pwac = data;

	if (!props || !type) {
		return;
	}

	if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
		const char *node_name, *media_class;
		if (!(node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME)) ||
			!(media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS))) {
			return;
		}

		/** Target device */
		if ((pwac->capture_type == PIPEWIRE_AUDIO_CAPTURE_INPUT &&
			 (strcmp(media_class, "Audio/Source") == 0 || strcmp(media_class, "Audio/Source/Virtual") == 0)) ||
			(pwac->capture_type == PIPEWIRE_AUDIO_CAPTURE_OUTPUT && strcmp(media_class, "Audio/Sink") == 0)) {
			const char *node_friendly_name = spa_dict_lookup(props, PW_KEY_NODE_NICK);
			if (!node_friendly_name) {
				node_friendly_name = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
				if (!node_friendly_name) {
					node_friendly_name = node_name;
				}
			}

			register_target_node(pwac, node_friendly_name, node_name, id);
		}
	} else if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
		const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
		if (!name || strcmp(name, "default") != 0) {
			return;
		}

		if (!obs_pw_audio_default_node_metadata_listen(&pwac->default_info.metadata, &pwac->pw, id,
													   pwac->capture_type == PIPEWIRE_AUDIO_CAPTURE_OUTPUT,
													   default_node_cb, pwac)) {
			blog(LOG_WARNING, "[pipewire] Failed to get default metadata, cannot detect default audio devices");
		}
	}
}

static void on_global_remove_cb(void *data, uint32_t id)
{
	struct obs_pw_audio_capture *pwac = data;

	if (pwac->default_info.node_id == id) {
		pwac->default_info.node_id = SPA_ID_INVALID;
	}

	/** If the node we're connected to is removed,
	  * try to find one with the same name and connect to it. */
	if (id == pwac->connected_id) {
		pwac->connected_id = SPA_ID_INVALID;

		pw_stream_disconnect(pwac->audio.stream);

		if (!pwac->default_info.autoconnect && !dstr_is_empty(&pwac->target_name)) {
			start_streaming(pwac, get_node_by_name(pwac, pwac->target_name.array));
		}
	}
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global_cb,
	.global_remove = on_global_remove_cb,
};
/* ------------------------------------------------- */

/* Source */
static void *pipewire_audio_capture_create(obs_data_t *settings, obs_source_t *source,
										   enum obs_pw_audio_capture_type capture_type)
{
	struct obs_pw_audio_capture *pwac = bzalloc(sizeof(struct obs_pw_audio_capture));

	if (!obs_pw_audio_instance_init(&pwac->pw)) {
		pw_thread_loop_lock(pwac->pw.thread_loop);
		obs_pw_audio_instance_destroy(&pwac->pw);

		bfree(pwac);
		return NULL;
	}

	pwac->source = source;
	pwac->capture_type = capture_type;
	pwac->default_info.node_id = SPA_ID_INVALID;
	pwac->connected_id = SPA_ID_INVALID;

	spa_list_init(&pwac->targets);

	if (obs_data_get_int(settings, "TargetId") != PW_ID_ANY) {
		/** Reset id setting, PipeWire node ids may not persist between sessions.
		  * Connecting to saved target will happen based on the TargetName setting
		  * once target has connected */
		obs_data_set_int(settings, "TargetId", 0);
	} else {
		pwac->default_info.autoconnect = true;
	}

	dstr_init_copy(&pwac->target_name, obs_data_get_string(settings, "TargetName"));

	pw_thread_loop_lock(pwac->pw.thread_loop);

	pw_registry_add_listener(pwac->pw.registry, &pwac->pw.registry_listener, &registry_events, pwac);

	struct pw_properties *props = obs_pw_audio_stream_properties(capture_type == PIPEWIRE_AUDIO_CAPTURE_OUTPUT, true);
	if (obs_pw_audio_stream_init(&pwac->audio, &pwac->pw, props, pwac->source)) {
		blog(LOG_INFO, "[pipewire] Created stream %p", pwac->audio.stream);
	} else {
		blog(LOG_WARNING, "[pipewire] Failed to create stream");
	}

	obs_pw_audio_instance_sync(&pwac->pw);
	pw_thread_loop_wait(pwac->pw.thread_loop);
	pw_thread_loop_unlock(pwac->pw.thread_loop);

	return pwac;
}

static void *pipewire_audio_capture_input_create(obs_data_t *settings, obs_source_t *source)
{
	return pipewire_audio_capture_create(settings, source, PIPEWIRE_AUDIO_CAPTURE_INPUT);
}
static void *pipewire_audio_capture_output_create(obs_data_t *settings, obs_source_t *source)
{
	return pipewire_audio_capture_create(settings, source, PIPEWIRE_AUDIO_CAPTURE_OUTPUT);
}

static void pipewire_audio_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "TargetId", PW_ID_ANY);
}

static obs_properties_t *pipewire_audio_capture_properties(void *data)
{
	struct obs_pw_audio_capture *pwac = data;

	obs_properties_t *p = obs_properties_create();

	obs_property_t *targets_list =
		obs_properties_add_list(p, "TargetId", obs_module_text("Device"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(targets_list, obs_module_text("Default"), PW_ID_ANY);

	pw_thread_loop_lock(pwac->pw.thread_loop);

	struct target_node *n;
	spa_list_for_each(n, &pwac->targets, obj.link)
	{
		obs_property_list_add_int(targets_list, n->friendly_name, n->id);
	}

	pw_thread_loop_unlock(pwac->pw.thread_loop);

	return p;
}

static void pipewire_audio_capture_update(void *data, obs_data_t *settings)
{
	struct obs_pw_audio_capture *pwac = data;

	uint32_t new_node_id = obs_data_get_int(settings, "TargetId");

	pw_thread_loop_lock(pwac->pw.thread_loop);

	if (new_node_id == PW_ID_ANY) {
		pwac->default_info.autoconnect = true;

		if (pwac->default_info.node_id != SPA_ID_INVALID) {
			start_streaming(pwac, get_node_by_id(pwac, pwac->default_info.node_id));
		}
		goto unlock;
	}

	pwac->default_info.autoconnect = false;

	struct target_node *new_node = get_node_by_id(pwac, new_node_id);
	if (!new_node) {
		goto unlock;
	}

	start_streaming(pwac, new_node);

	obs_data_set_string(settings, "TargetName", pwac->target_name.array);

unlock:
	pw_thread_loop_unlock(pwac->pw.thread_loop);
}

static void pipewire_audio_capture_show(void *data)
{
	struct obs_pw_audio_capture *pwac = data;

	if (pwac->audio.stream) {
		pw_stream_set_active(pwac->audio.stream, true);
	}
}

static void pipewire_audio_capture_hide(void *data)
{
	struct obs_pw_audio_capture *pwac = data;

	if (pwac->audio.stream) {
		pw_stream_set_active(pwac->audio.stream, false);
	}
}

static void pipewire_audio_capture_destroy(void *data)
{
	struct obs_pw_audio_capture *pwac = data;

	pw_thread_loop_lock(pwac->pw.thread_loop);

	struct target_node *n, *tn;
	spa_list_for_each_safe(n, tn, &pwac->targets, obj.link)
	{
		pw_proxy_destroy(n->obj.proxy);
	}

	obs_pw_audio_stream_destroy(&pwac->audio);

	if (pwac->default_info.metadata.proxy) {
		pw_proxy_destroy(pwac->default_info.metadata.proxy);
	}

	obs_pw_audio_instance_destroy(&pwac->pw);

	dstr_free(&pwac->default_info.name);
	dstr_free(&pwac->target_name);

	bfree(pwac);
}

static const char *pipewire_audio_capture_input_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("PipeWireAudioCaptureInput");
}
static const char *pipewire_audio_capture_output_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("PipeWireAudioCaptureOutput");
}

void pipewire_audio_capture_load(void)
{
	const struct obs_source_info pipewire_audio_capture_input = {
		.id = "pipewire_audio_input_capture",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
		.get_name = pipewire_audio_capture_input_name,
		.create = pipewire_audio_capture_input_create,
		.get_defaults = pipewire_audio_capture_defaults,
		.get_properties = pipewire_audio_capture_properties,
		.update = pipewire_audio_capture_update,
		.show = pipewire_audio_capture_show,
		.hide = pipewire_audio_capture_hide,
		.destroy = pipewire_audio_capture_destroy,
		.icon_type = OBS_ICON_TYPE_AUDIO_INPUT,
	};
	const struct obs_source_info pipewire_audio_capture_output = {
		.id = "pipewire_audio_output_capture",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_DO_NOT_SELF_MONITOR,
		.get_name = pipewire_audio_capture_output_name,
		.create = pipewire_audio_capture_output_create,
		.get_defaults = pipewire_audio_capture_defaults,
		.get_properties = pipewire_audio_capture_properties,
		.update = pipewire_audio_capture_update,
		.show = pipewire_audio_capture_show,
		.hide = pipewire_audio_capture_hide,
		.destroy = pipewire_audio_capture_destroy,
		.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT,
	};

	obs_register_source(&pipewire_audio_capture_input);
	obs_register_source(&pipewire_audio_capture_output);
}
