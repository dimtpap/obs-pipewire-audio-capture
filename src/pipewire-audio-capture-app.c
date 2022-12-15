/* pipewire-audio-capture-apps.c
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

/* Source for capturing applciation audio using PipeWire */

struct target_node_port {
	const char *channel;
	uint32_t id;

	struct obs_pw_audio_proxied_object obj;
};

struct target_node {
	const char *name;
	const char *app_name;
	const char *binary;
	uint32_t id;
	struct spa_list ports;
	size_t *p_n_targets;

	struct spa_hook node_listener;

	struct obs_pw_audio_proxied_object obj;
};

struct system_sink {
	const char *name;
	uint32_t id;

	struct obs_pw_audio_proxied_object obj;
};

struct capture_sink_link {
	uint32_t id;

	struct obs_pw_audio_proxied_object obj;
};

struct capture_sink_port {
	const char *channel;
	uint32_t id;
};

struct obs_pw_audio_capture_app {
	struct obs_pw_audio_instance pw;

	struct obs_pw_audio_stream audio;

	/** The app capture sink automatically mixes
	  * the audio of all the app streams */
	struct {
		struct pw_proxy *proxy;
		struct spa_hook proxy_listener;
		bool autoconnect_targets;
		uint32_t id;
		uint32_t channels;
		struct dstr position;
		DARRAY(struct capture_sink_port) ports;

		/* Links between app streams and the capture sink */
		struct spa_list links;
	} sink;

	/** Need the default system sink to create
	  * the app capture sink with the same audio channels */
	struct spa_list system_sinks;
	struct {
		struct obs_pw_audio_default_node_metadata metadata;
		struct pw_proxy *sink;
		struct spa_hook sink_listener;
		struct spa_hook sink_proxy_listener;
	} default_info;

	struct spa_list targets;
	size_t n_targets;

	struct dstr target;
	bool except_app;
};

/* System sinks */
static void system_sink_destroy_cb(void *data)
{
	struct system_sink *s = data;
	bfree((void *)s->name);
}

static void register_system_sink(struct obs_pw_audio_capture_app *pwac, uint32_t global_id, const char *name)
{
	struct pw_proxy *sink_proxy =
		pw_registry_bind(pwac->pw.registry, global_id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
	if (!sink_proxy) {
		return;
	}

	struct system_sink *sink = bmalloc(sizeof(struct system_sink));
	sink->name = bstrdup(name);
	sink->id = global_id;

	obs_pw_audio_proxied_object_init(&sink->obj, sink_proxy, &pwac->system_sinks, NULL, system_sink_destroy_cb, sink);
}
/* ------------------------------------------------- */

/* Target nodes and ports */
static void port_destroy_cb(void *data)
{
	struct target_node_port *p = data;
	bfree((void *)p->channel);
}

static void node_destroy_cb(void *data)
{
	struct target_node *node = data;

	spa_hook_remove(&node->node_listener);

	struct target_node_port *p, *tp;
	spa_list_for_each_safe(p, tp, &node->ports, obj.link)
	{
		pw_proxy_destroy(p->obj.proxy);
	}

	(*node->p_n_targets)--;

	bfree((void *)node->binary);
	bfree((void *)node->app_name);
	bfree((void *)node->name);
}

static struct target_node_port *node_register_port(struct target_node *node, uint32_t global_id,
												   struct pw_registry *registry, const char *channel)
{
	struct pw_proxy *port_proxy = pw_registry_bind(registry, global_id, PW_TYPE_INTERFACE_Port, PW_VERSION_PORT, 0);
	if (!port_proxy) {
		return NULL;
	}

	struct target_node_port *port = bmalloc(sizeof(struct target_node_port));
	port->channel = bstrdup(channel);
	port->id = global_id;

	obs_pw_audio_proxied_object_init(&port->obj, port_proxy, &node->ports, NULL, port_destroy_cb, port);

	return port;
}

static void on_node_info_cb(void *data, const struct pw_node_info *info)
{
	if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) == 0 || !info->props || !info->props->n_items) {
		return;
	}

	const char *binary = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_BINARY);
	if (!binary) {
		return;
	}

	struct target_node *node = data;
	bfree((void *)node->binary);
	node->binary = bstrdup(binary);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = on_node_info_cb,
};

static void register_target_node(struct obs_pw_audio_capture_app *pwac, uint32_t global_id, const char *app_name,
								 const char *name)
{
	struct pw_proxy *node_proxy =
		pw_registry_bind(pwac->pw.registry, global_id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
	if (!node_proxy) {
		return;
	}

	struct target_node *node = bmalloc(sizeof(struct target_node));
	node->name = bstrdup(name);
	node->app_name = bstrdup(app_name);
	node->binary = NULL;
	node->id = global_id;
	node->p_n_targets = &pwac->n_targets;
	spa_list_init(&node->ports);

	pwac->n_targets++;

	obs_pw_audio_proxied_object_init(&node->obj, node_proxy, &pwac->targets, NULL, node_destroy_cb, node);
	pw_proxy_add_object_listener(node_proxy, &node->node_listener, &node_events, node);
}

static bool node_is_targeted(struct obs_pw_audio_capture_app *pwac, struct target_node *node)
{
	if (dstr_is_empty(&pwac->target)) {
		return false;
	}

	return (dstr_cmpi(&pwac->target, node->binary) == 0 || dstr_cmpi(&pwac->target, node->app_name) == 0 ||
			dstr_cmpi(&pwac->target, node->name) == 0) ^
		   pwac->except_app;
}
/* ------------------------------------------------- */

/* App streams <-> Capture sink links */
static void link_bound_cb(void *data, uint32_t global_id)
{
	struct capture_sink_link *link = data;
	link->id = global_id;
}

static void link_destroy_cb(void *data)
{
	struct capture_sink_link *link = data;
	blog(LOG_DEBUG, "[pipewire] Link %u destroyed", link->id);
}

static void link_port_to_sink(struct obs_pw_audio_capture_app *pwac, struct target_node_port *port, uint32_t node_id)
{
	blog(LOG_DEBUG, "[pipewire] Connecting port %u of node %u to app capture sink", port->id, node_id);

	uint32_t p = 0;
	if (pwac->sink.channels == 1 && /* Mono capture sink */
		pwac->sink.ports.num >= 1) {
		p = pwac->sink.ports.array[0].id;
	} else {
		for (size_t i = 0; i < pwac->sink.ports.num; i++) {
			if (astrcmpi(pwac->sink.ports.array[i].channel, port->channel) == 0) {
				p = pwac->sink.ports.array[i].id;
				break;
			}
		}
	}

	if (!p) {
		blog(
			LOG_WARNING,
			"[pipewire] Could not connect port %u of node %u to app capture sink. No port of app capture sink has channel %s",
			port->id, node_id, port->channel);
		return;
	}

	struct pw_properties *link_props =
		pw_properties_new(PW_KEY_OBJECT_LINGER, "false", PW_KEY_FACTORY_NAME, "link-factory", NULL);

	pw_properties_setf(link_props, PW_KEY_LINK_OUTPUT_NODE, "%u", node_id);
	pw_properties_setf(link_props, PW_KEY_LINK_OUTPUT_PORT, "%u", port->id);

	pw_properties_setf(link_props, PW_KEY_LINK_INPUT_NODE, "%u", pwac->sink.id);
	pw_properties_setf(link_props, PW_KEY_LINK_INPUT_PORT, "%u", p);

	struct pw_proxy *link_proxy = pw_core_create_object(pwac->pw.core, "link-factory", PW_TYPE_INTERFACE_Link,
														PW_VERSION_LINK, &link_props->dict, 0);

	pw_properties_free(link_props);

	if (!link_proxy) {
		blog(LOG_WARNING, "[pipewire] Could not connect port %u of node %u to app capture sink", port->id, node_id);
		return;
	}

	struct capture_sink_link *link = bmalloc(sizeof(struct capture_sink_link));
	link->id = SPA_ID_INVALID;

	obs_pw_audio_proxied_object_init(&link->obj, link_proxy, &pwac->sink.links, link_bound_cb, link_destroy_cb, link);

	obs_pw_audio_instance_sync(&pwac->pw);
}

static void link_node_to_sink(struct obs_pw_audio_capture_app *pwac, struct target_node *node)
{
	struct target_node_port *p;
	spa_list_for_each(p, &node->ports, obj.link)
	{
		link_port_to_sink(pwac, p, node->id);
	}
}
/* ------------------------------------------------- */

/* App capture sink */

/** The app capture sink is created when there
  * is info about the system's default sink.
  * See the on_metadata and on_default_sink callbacks */

static void on_sink_proxy_bound_cb(void *data, uint32_t global_id)
{
	struct obs_pw_audio_capture_app *pwac = data;
	pwac->sink.id = global_id;
	da_init(pwac->sink.ports);
}

static void on_sink_proxy_removed_cb(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;
	blog(LOG_WARNING, "[pipewire] App capture sink %u has been destroyed by the PipeWire remote", pwac->sink.id);
	pw_proxy_destroy(pwac->sink.proxy);
}

static void on_sink_proxy_destroy_cb(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;

	spa_hook_remove(&pwac->sink.proxy_listener);
	spa_zero(pwac->sink.proxy_listener);

	for (size_t i = 0; i < pwac->sink.ports.num; i++) {
		struct capture_sink_port *p = &pwac->sink.ports.array[i];
		bfree((void *)p->channel);
	}
	da_free(pwac->sink.ports);

	pwac->sink.channels = 0;
	dstr_free(&pwac->sink.position);

	pwac->sink.autoconnect_targets = false;
	pwac->sink.proxy = NULL;

	blog(LOG_DEBUG, "[pipewire] App capture sink %u destroyed", pwac->sink.id);

	pwac->sink.id = SPA_ID_INVALID;
}

static void on_sink_proxy_error_cb(void *data, int seq, int res, const char *message)
{
	UNUSED_PARAMETER(data);
	blog(LOG_ERROR, "[pipewire] App capture sink error: seq:%d res:%d :%s", seq, res, message);
}

static const struct pw_proxy_events sink_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.bound = on_sink_proxy_bound_cb,
	.removed = on_sink_proxy_removed_cb,
	.destroy = on_sink_proxy_destroy_cb,
	.error = on_sink_proxy_error_cb,
};

static void register_capture_sink_port(struct obs_pw_audio_capture_app *pwac, uint32_t global_id, const char *channel)
{
	struct capture_sink_port *port = da_push_back_new(pwac->sink.ports);
	port->channel = bstrdup(channel);
	port->id = global_id;
}

static void destroy_sink_links(struct obs_pw_audio_capture_app *pwac)
{
	struct capture_sink_link *l, *t;
	spa_list_for_each_safe(l, t, &pwac->sink.links, obj.link)
	{
		pw_proxy_destroy(l->obj.proxy);
	}
}

static void connect_targets(struct obs_pw_audio_capture_app *pwac, const char *target, bool except)
{
	pwac->except_app = except;

	if (target) {
		dstr_copy(&pwac->target, target);
	}

	if (!pwac->sink.proxy) {
		return;
	}

	destroy_sink_links(pwac);

	if (dstr_is_empty(&pwac->target)) {
		return;
	}

	struct target_node *n;
	spa_list_for_each(n, &pwac->targets, obj.link)
	{
		if (node_is_targeted(pwac, n)) {
			link_node_to_sink(pwac, n);
		}
	}
}

static bool make_capture_sink(struct obs_pw_audio_capture_app *pwac, uint32_t channels, const char *position)
{
	struct pw_properties *sink_props =
		pw_properties_new(PW_KEY_NODE_NAME, "OBS", PW_KEY_NODE_DESCRIPTION, "OBS App Audio Capture Sink",
						  PW_KEY_FACTORY_NAME, "support.null-audio-sink", PW_KEY_MEDIA_CLASS, "Audio/Sink/Virtual",
						  PW_KEY_NODE_VIRTUAL, "true", SPA_KEY_AUDIO_POSITION, position, NULL);

	pw_properties_setf(sink_props, PW_KEY_AUDIO_CHANNELS, "%u", channels);

	pwac->sink.proxy =
		pw_core_create_object(pwac->pw.core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &sink_props->dict, 0);

	pw_properties_free(sink_props);

	if (!pwac->sink.proxy) {
		blog(LOG_WARNING, "[pipewire] Failed to create app capture sink");
		return false;
	}

	pwac->sink.channels = channels;
	dstr_copy(&pwac->sink.position, position);

	pwac->sink.id = SPA_ID_INVALID;

	pw_proxy_add_listener(pwac->sink.proxy, &pwac->sink.proxy_listener, &sink_proxy_events, pwac);

	obs_pw_audio_instance_sync(&pwac->pw);

	while (pwac->sink.id == SPA_ID_INVALID || pwac->sink.ports.num != channels) {
		/* Iterate until the sink is bound and all the ports are registered */
		pw_loop_iterate(pw_thread_loop_get_loop(pwac->pw.thread_loop), -1);
	}

	blog(LOG_INFO, "[pipewire] Created app capture sink %u with %u channels and position %s", pwac->sink.id, channels,
		 position);

	connect_targets(pwac, NULL, pwac->except_app);

	pwac->sink.autoconnect_targets = true;

	if (!pwac->audio.stream) {
		return true;
	}

	if (obs_pw_audio_stream_connect(
			&pwac->audio, PW_DIRECTION_INPUT, pwac->sink.id,
			PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DONT_RECONNECT, channels) < 0) {
		blog(LOG_WARNING, "[pipewire] Error connecting stream %p to app capture sink %u", pwac->audio.stream,
			 pwac->sink.id);
	}

	return true;
}

static void destroy_capture_sink(struct obs_pw_audio_capture_app *pwac)
{
	/* Links are automatically destroyed by PipeWire */

	if (!pwac->sink.proxy) {
		return;
	}

	if (pwac->audio.stream) {
		pw_stream_disconnect(pwac->audio.stream);
	}
	pwac->sink.autoconnect_targets = false;
	pw_proxy_destroy(pwac->sink.proxy);
	obs_pw_audio_instance_sync(&pwac->pw);
}
/* ------------------------------------------------- */

/* Default system sink */
static void on_default_sink_info_cb(void *data, const struct pw_node_info *info)
{
	if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) == 0 || !info->props || !info->props->n_items) {
		return;
	}

	struct obs_pw_audio_capture_app *pwac = data;

	/** Use stereo if
	  * - The default sink uses the Pro Audio profile, since all streams will be configured to use stereo
	  * https://gitlab.freedesktop.org/pipewire/pipewire/-/wikis/FAQ#what-is-the-pro-audio-profile
	  * - The default sink doesn't have the needed props and there isn't already an app capture sink */

	const char *channels = spa_dict_lookup(info->props, PW_KEY_AUDIO_CHANNELS);
	const char *position = spa_dict_lookup(info->props, SPA_KEY_AUDIO_POSITION);
	if (!channels || !position) {
		if (pwac->sink.proxy) {
			return;
		}
		channels = "2";
		position = "FL,FR";
	} else if (astrstri(position, "AUX")) {
		/* Pro Audio sinks use AUX0,AUX1... and so on as their position (see link above) */
		channels = "2";
		position = "FL,FR";
	}

	uint32_t c = strtoul(channels, NULL, 10);
	if (!c) {
		return;
	}

	/* No need to create a new capture sink if the channels are the same */
	if (pwac->sink.channels == c && !dstr_is_empty(&pwac->sink.position) &&
		dstr_cmp(&pwac->sink.position, position) == 0) {
		return;
	}

	destroy_capture_sink(pwac);

	make_capture_sink(pwac, c, position);
}

static const struct pw_node_events default_sink_events = {
	PW_VERSION_NODE_EVENTS,
	.info = on_default_sink_info_cb,
};

static void on_default_sink_proxy_removed_cb(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;
	pw_proxy_destroy(pwac->default_info.sink);
}

static void on_default_sink_proxy_destroy_cb(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;
	spa_hook_remove(&pwac->default_info.sink_proxy_listener);
	spa_zero(pwac->default_info.sink_proxy_listener);

	pwac->default_info.sink = NULL;
}

static const struct pw_proxy_events default_sink_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = on_default_sink_proxy_removed_cb,
	.destroy = on_default_sink_proxy_destroy_cb,
};

static void default_node_cb(void *data, const char *name)
{
	struct obs_pw_audio_capture_app *pwac = data;

	blog(LOG_DEBUG, "[pipewire] New default sink %s", name);

	/* Find the new default sink and bind to it to get its channel info */
	struct system_sink *t, *s = NULL;
	spa_list_for_each(t, &pwac->system_sinks, obj.link)
	{
		if (strcmp(name, t->name) == 0) {
			s = t;
			break;
		}
	}
	if (!s) {
		return;
	}

	if (pwac->default_info.sink) {
		pw_proxy_destroy(pwac->default_info.sink);
	}

	pwac->default_info.sink = pw_registry_bind(pwac->pw.registry, s->id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
	if (!pwac->default_info.sink) {
		if (!pwac->sink.proxy) {
			blog(LOG_WARNING, "[pipewire] Failed to get default sink info, app capture sink defaulting to stereo");
			make_capture_sink(pwac, 2, "FL,FR");
		}
		return;
	}

	pw_proxy_add_object_listener(pwac->default_info.sink, &pwac->default_info.sink_listener, &default_sink_events,
								 pwac);
	pw_proxy_add_listener(pwac->default_info.sink, &pwac->default_info.sink_proxy_listener, &default_sink_proxy_events,
						  pwac);
}
/* ------------------------------------------------- */

/* Registry */
static void on_global_cb(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version,
						 const struct spa_dict *props)
{
	UNUSED_PARAMETER(permissions);
	UNUSED_PARAMETER(version);

	if (!props || !type) {
		return;
	}

	struct obs_pw_audio_capture_app *pwac = data;

	if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
		const char *nid, *dir, *chn;
		if (!(nid = spa_dict_lookup(props, PW_KEY_NODE_ID)) || !(dir = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)) ||
			!(chn = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL))) {
			return;
		}

		uint32_t node_id = strtoul(nid, NULL, 10);

		if (astrcmpi(dir, "in") == 0 && node_id == pwac->sink.id) {
			register_capture_sink_port(pwac, id, chn);
		} else if (astrcmpi(dir, "out") == 0) {
			/* Possibly a target port */
			struct target_node *t, *n = NULL;
			spa_list_for_each(t, &pwac->targets, obj.link)
			{
				if (t->id == node_id) {
					n = t;
					break;
				}
			}
			if (!n) {
				return;
			}

			struct target_node_port *p = node_register_port(n, id, pwac->pw.registry, chn);

			if (p && pwac->sink.autoconnect_targets && node_is_targeted(pwac, n)) {
				link_port_to_sink(pwac, p, n->id);
			}
		}
	} else if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
		const char *node_name, *media_class;
		if (!(node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME)) ||
			!(media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS))) {
			return;
		}

		if (strcmp(media_class, "Stream/Output/Audio") == 0) {
			/* Target node */
			const char *node_app_name = spa_dict_lookup(props, PW_KEY_APP_NAME);

			if (!node_app_name) {
				node_app_name = node_name;
			}

			register_target_node(pwac, id, node_app_name, node_name);
		} else if (strcmp(media_class, "Audio/Sink") == 0) {
			register_system_sink(pwac, id, node_name);
		}
	} else if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
		const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
		if (!name || strcmp(name, "default") != 0) {
			return;
		}

		if (!obs_pw_audio_default_node_metadata_listen(&pwac->default_info.metadata, &pwac->pw, id, true,
													   default_node_cb, pwac) &&
			!pwac->sink.proxy) {
			blog(LOG_WARNING, "[pipewire] Failed to get default metadata, app capture sink defaulting to stereo");
			make_capture_sink(pwac, 2, "FL,FR");
		}
	}
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global_cb,
};
/* ------------------------------------------------- */

/* Source */
static void *pipewire_audio_capture_app_create(obs_data_t *settings, obs_source_t *source)
{
	struct obs_pw_audio_capture_app *pwac = bzalloc(sizeof(struct obs_pw_audio_capture_app));

	if (!obs_pw_audio_instance_init(&pwac->pw)) {
		obs_pw_audio_instance_destroy(&pwac->pw);

		bfree(pwac);
		return NULL;
	}

	spa_list_init(&pwac->targets);
	spa_list_init(&pwac->sink.links);
	spa_list_init(&pwac->system_sinks);

	pwac->sink.id = SPA_ID_INVALID;
	dstr_init(&pwac->sink.position);

	dstr_init_copy(&pwac->target, obs_data_get_string(settings, "TargetName"));
	pwac->except_app = obs_data_get_bool(settings, "ExceptApp");

	pw_registry_add_listener(pwac->pw.registry, &pwac->pw.registry_listener, &registry_events, pwac);

	struct pw_properties *stream_props = obs_pw_audio_stream_properties(true, false);
	if (obs_pw_audio_stream_init(&pwac->audio, &pwac->pw, stream_props, source)) {
		blog(LOG_INFO, "[pipewire] Created stream %p", pwac->audio.stream);
	} else {
		blog(LOG_WARNING, "[pipewire] Failed to create stream");
	}

	obs_pw_audio_instance_sync(&pwac->pw);
	pw_thread_loop_wait(pwac->pw.thread_loop);
	pw_thread_loop_unlock(pwac->pw.thread_loop);

	return pwac;
}

static void pipewire_audio_capture_app_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "ExceptApp", false);
}

static int cmp_targets(const void *a, const void *b)
{
	const char *a_str = *(char **)a;
	const char *b_str = *(char **)b;
	return strcmp(a_str, b_str);
}

static obs_properties_t *pipewire_audio_capture_app_properties(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;

	obs_properties_t *p = obs_properties_create();

	obs_property_t *targets_prop_list = obs_properties_add_list(p, "TargetName", obs_module_text("Application"),
																OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);

	obs_properties_add_bool(p, "ExceptApp", obs_module_text("ExceptApp"));

	DARRAY(char *) targets_arr;
	da_init(targets_arr);

	pw_thread_loop_lock(pwac->pw.thread_loop);

	da_reserve(targets_arr, pwac->n_targets);

	struct target_node *node;
	spa_list_for_each(node, &pwac->targets, obj.link)
	{
		da_push_back(targets_arr, node->binary ? &node->binary : node->app_name ? &node->app_name : &node->name);
	}

	/* Show just one entry per app */

	qsort(targets_arr.array, targets_arr.num, sizeof(char *), cmp_targets);

	for (size_t i = 0; i < targets_arr.num; i++) {
		if (i == 0 || strcmp(targets_arr.array[i - 1], targets_arr.array[i]) != 0) {
			obs_property_list_add_string(targets_prop_list, targets_arr.array[i], NULL);
		}
	}

	pw_thread_loop_unlock(pwac->pw.thread_loop);

	da_free(targets_arr);

	return p;
}

static void pipewire_audio_capture_app_update(void *data, obs_data_t *settings)
{
	struct obs_pw_audio_capture_app *pwac = data;

	bool except = obs_data_get_bool(settings, "ExceptApp");

	const char *new_target = obs_data_get_string(settings, "TargetName");

	pw_thread_loop_lock(pwac->pw.thread_loop);

	if (except == pwac->except_app && dstr_cmpi(&pwac->target, new_target) == 0) {
		pw_thread_loop_unlock(pwac->pw.thread_loop);
		return;
	}

	connect_targets(pwac, new_target, except);

	obs_pw_audio_instance_sync(&pwac->pw);
	pw_thread_loop_wait(pwac->pw.thread_loop);
	pw_thread_loop_unlock(pwac->pw.thread_loop);
}

static void pipewire_audio_capture_app_show(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;

	if (pwac->audio.stream) {
		pw_stream_set_active(pwac->audio.stream, true);
	}
}

static void pipewire_audio_capture_app_hide(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;

	if (pwac->audio.stream) {
		pw_stream_set_active(pwac->audio.stream, false);
	}
}

static void pipewire_audio_capture_app_destroy(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;

	pw_thread_loop_lock(pwac->pw.thread_loop);

	struct target_node *n, *tn;
	spa_list_for_each_safe(n, tn, &pwac->targets, obj.link)
	{
		pw_proxy_destroy(n->obj.proxy);
	}
	struct system_sink *s, *ts;
	spa_list_for_each_safe(s, ts, &pwac->system_sinks, obj.link)
	{
		pw_proxy_destroy(s->obj.proxy);
	}

	obs_pw_audio_stream_destroy(&pwac->audio);

	destroy_capture_sink(pwac);

	if (pwac->default_info.sink) {
		pw_proxy_destroy(pwac->default_info.sink);
	}
	if (pwac->default_info.metadata.proxy) {
		pw_proxy_destroy(pwac->default_info.metadata.proxy);
	}

	obs_pw_audio_instance_destroy(&pwac->pw);

	dstr_free(&pwac->sink.position);
	dstr_free(&pwac->target);

	bfree(pwac);
}

static const char *pipewire_audio_capture_app_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("PipeWireAudioCaptureApplication");
}

void pipewire_audio_capture_app_load(void)
{
	const struct obs_source_info pipewire_audio_capture_application = {
		.id = "pipewire_audio_application_capture",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
		.get_name = pipewire_audio_capture_app_name,
		.create = pipewire_audio_capture_app_create,
		.get_defaults = pipewire_audio_capture_app_defaults,
		.get_properties = pipewire_audio_capture_app_properties,
		.update = pipewire_audio_capture_app_update,
		.show = pipewire_audio_capture_app_show,
		.hide = pipewire_audio_capture_app_hide,
		.destroy = pipewire_audio_capture_app_destroy,
		.icon_type = OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT,
	};

	obs_register_source(&pipewire_audio_capture_application);
}
