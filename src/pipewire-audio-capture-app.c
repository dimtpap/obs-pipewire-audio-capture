/* pipewire-audio-capture-app.c
 *
 * Copyright 2022-2024 Dimitris Papaioannou <dimtpap@protonmail.com>
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
};

struct target_node {
	const char *name;
	const char *app_name;
	const char *binary;
	uint32_t client_id;
	uint32_t id;
	struct obs_pw_audio_proxy_list ports;
	uint32_t *p_n_nodes;

	struct spa_hook node_listener;
};

struct target_client {
	const char *app_name;
	const char *binary;
	uint32_t id;

	struct spa_hook client_listener;
};

struct system_sink {
	const char *name;
	uint32_t id;
};

struct capture_sink_link {
	uint32_t id;
};

struct capture_sink_port {
	const char *channel;
	uint32_t id;
};

enum capture_mode { CAPTURE_MODE_SINGLE, CAPTURE_MODE_MULTIPLE };
enum match_priority { MATCH_PRIORITY_BINARY_NAME, MATCH_PRIORITY_APP_NAME };

#define SETTING_CAPTURE_MODE "CaptureMode"
#define SETTING_MATCH_PRIORITY "MatchPriorty"
#define SETTING_EXCLUDE_SELECTIONS "ExceptApp"
#define SETTING_SELECTION_SINGLE "TargetName"
#define SETTING_SELECTION_MULTIPLE "apps"
#define SETTING_AVAILABLE_APPS "AppToAdd"
#define SETTING_ADD_TO_SELECTIONS "AddToSelected"

/** This source basically works like this:
    - Keep track of output streams and their ports, system sinks and the default sink

    - Keep track of the channels of the default system sink and create a new virtual sink,
      destroying the previously made one, with the same channels, then connect the stream to it

    - Connect any registered or new stream ports to the sink
*/
struct obs_pw_audio_capture_app {
	obs_source_t *source;

	struct obs_pw_audio_instance pw;

	/** The app capture sink automatically mixes
	  * the audio of all the app streams */
	struct {
		struct pw_proxy *proxy;
		struct spa_hook proxy_listener;
		bool autoconnect_targets;
		uint32_t id;
		uint32_t serial;
		uint32_t channels;
		struct dstr position;
		DARRAY(struct capture_sink_port) ports;

		/* Links between app streams and the capture sink */
		struct obs_pw_audio_proxy_list links;
	} sink;

	/** Need the default system sink to create
	  * the app capture sink with the same audio channels */
	struct obs_pw_audio_proxy_list system_sinks;
	struct {
		struct obs_pw_audio_default_node_metadata metadata;
		struct pw_proxy *proxy;
		struct spa_hook node_listener;
		struct spa_hook proxy_listener;
	} default_sink;

	struct obs_pw_audio_proxy_list clients;

	struct obs_pw_audio_proxy_list nodes;
	uint32_t n_nodes;

	enum capture_mode capture_mode;
	enum match_priority match_priority;
	bool except;
	DARRAY(const char *) selections;
};

/* System sinks */
static void system_sink_destroy_cb(void *data)
{
	struct system_sink *s = data;
	bfree((void *)s->name);
}

static void register_system_sink(struct obs_pw_audio_capture_app *pwac, uint32_t global_id, const char *name)
{
	struct pw_proxy *sink_proxy = pw_registry_bind(pwac->pw.registry, global_id, PW_TYPE_INTERFACE_Node,
												   PW_VERSION_NODE, sizeof(struct system_sink));
	if (!sink_proxy) {
		return;
	}

	struct system_sink *sink = pw_proxy_get_user_data(sink_proxy);
	sink->name = bstrdup(name);
	sink->id = global_id;

	obs_pw_audio_proxy_list_append(&pwac->system_sinks, sink_proxy);
}
/* ------------------------------------------------- */

/* Target clients */
static void client_destroy_cb(void *data)
{
	struct target_client *client = data;
	bfree((void *)client->app_name);
	bfree((void *)client->binary);

	spa_hook_remove(&client->client_listener);
}

static void on_client_info_cb(void *data, const struct pw_client_info *info)
{
	if ((info->change_mask & PW_CLIENT_CHANGE_MASK_PROPS) == 0 || !info->props || !info->props->n_items) {
		return;
	}

	const char *binary = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_BINARY);
	if (!binary) {
		return;
	}

	struct target_client *client = data;
	bfree((void *)client->binary);
	client->binary = bstrdup(binary);
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.info = on_client_info_cb,
};

static void register_target_client(struct obs_pw_audio_capture_app *pwac, uint32_t global_id, const char *app_name)
{
	struct pw_proxy *client_proxy = pw_registry_bind(pwac->pw.registry, global_id, PW_TYPE_INTERFACE_Client,
													 PW_VERSION_CLIENT, sizeof(struct target_client));
	if (!client_proxy) {
		return;
	}

	struct target_client *client = pw_proxy_get_user_data(client_proxy);
	client->binary = NULL;
	client->app_name = bstrdup(app_name);
	client->id = global_id;

	obs_pw_audio_proxy_list_append(&pwac->clients, client_proxy);
	pw_proxy_add_object_listener(client_proxy, &client->client_listener, &client_events, client);
}

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

	obs_pw_audio_proxy_list_clear(&node->ports);

	(*node->p_n_nodes)--;

	bfree((void *)node->binary);
	bfree((void *)node->app_name);
	bfree((void *)node->name);
}

static struct target_node_port *node_register_port(struct target_node *node, uint32_t global_id,
												   struct pw_registry *registry, const char *channel)
{
	struct pw_proxy *port_proxy =
		pw_registry_bind(registry, global_id, PW_TYPE_INTERFACE_Port, PW_VERSION_PORT, sizeof(struct target_node_port));
	if (!port_proxy) {
		return NULL;
	}

	struct target_node_port *port = pw_proxy_get_user_data(port_proxy);
	port->channel = bstrdup(channel);
	port->id = global_id;

	obs_pw_audio_proxy_list_append(&node->ports, port_proxy);

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

static void register_target_node(struct obs_pw_audio_capture_app *pwac, uint32_t global_id, uint32_t client_id,
								 const char *app_name, const char *name)
{
	struct pw_proxy *node_proxy = pw_registry_bind(pwac->pw.registry, global_id, PW_TYPE_INTERFACE_Node,
												   PW_VERSION_NODE, sizeof(struct target_node));
	if (!node_proxy) {
		return;
	}

	struct target_node *node = pw_proxy_get_user_data(node_proxy);
	node->name = bstrdup(name);
	node->app_name = bstrdup(app_name);
	node->binary = NULL;
	node->id = global_id;
	node->client_id = client_id;
	node->p_n_nodes = &pwac->n_nodes;
	obs_pw_audio_proxy_list_init(&node->ports, NULL, port_destroy_cb);

	pwac->n_nodes++;

	obs_pw_audio_proxy_list_append(&pwac->nodes, node_proxy);
	pw_proxy_add_object_listener(node_proxy, &node->node_listener, &node_events, node);
}

static bool node_is_targeted(struct obs_pw_audio_capture_app *pwac, struct target_node *node)
{
	bool targeted = false;
	for (size_t i = 0; i < pwac->selections.num && !targeted; i++) {
		const char *selection = pwac->selections.array[i];

		targeted = (astrcmpi(selection, node->binary) == 0 || astrcmpi(selection, node->app_name) == 0 ||
					astrcmpi(selection, node->name) == 0);

		if (!targeted && node->client_id) {
			struct obs_pw_audio_proxy_list_iter iter;
			obs_pw_audio_proxy_list_iter_init(&iter, &pwac->clients);

			struct target_client *client;
			while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&client)) {
				if (client->id == node->client_id) {
					targeted = (astrcmpi(selection, client->binary) == 0 || astrcmpi(selection, client->app_name) == 0);
					break;
				}
			}
		}
	}

	return targeted ^ pwac->except;
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

	struct pw_properties *link_props = pw_properties_new(PW_KEY_OBJECT_LINGER, "false", NULL);

	pw_properties_setf(link_props, PW_KEY_LINK_OUTPUT_NODE, "%u", node_id);
	pw_properties_setf(link_props, PW_KEY_LINK_OUTPUT_PORT, "%u", port->id);

	pw_properties_setf(link_props, PW_KEY_LINK_INPUT_NODE, "%u", pwac->sink.id);
	pw_properties_setf(link_props, PW_KEY_LINK_INPUT_PORT, "%u", p);

	struct pw_proxy *link_proxy = pw_core_create_object(pwac->pw.core, "link-factory", PW_TYPE_INTERFACE_Link,
														PW_VERSION_LINK, &link_props->dict,
														sizeof(struct capture_sink_link));

	obs_pw_audio_instance_sync(&pwac->pw);

	pw_properties_free(link_props);

	if (!link_proxy) {
		blog(LOG_WARNING, "[pipewire] Could not connect port %u of node %u to app capture sink", port->id, node_id);
		return;
	}

	struct capture_sink_link *link = pw_proxy_get_user_data(link_proxy);
	link->id = SPA_ID_INVALID;

	obs_pw_audio_proxy_list_append(&pwac->sink.links, link_proxy);
}

static void link_node_to_sink(struct obs_pw_audio_capture_app *pwac, struct target_node *node)
{
	struct obs_pw_audio_proxy_list_iter iter;
	obs_pw_audio_proxy_list_iter_init(&iter, &node->ports);

	struct target_node_port *port;
	while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&port)) {
		link_port_to_sink(pwac, port, node->id);
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
	obs_pw_audio_proxy_list_clear(&pwac->sink.links);
}

static void connect_targets(struct obs_pw_audio_capture_app *pwac)
{
	if (!pwac->sink.proxy) {
		return;
	}

	destroy_sink_links(pwac);

	if (pwac->selections.num == 0) {
		return;
	}

	struct obs_pw_audio_proxy_list_iter iter;
	obs_pw_audio_proxy_list_iter_init(&iter, &pwac->nodes);

	struct target_node *node;
	while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&node)) {
		if (node_is_targeted(pwac, node)) {
			link_node_to_sink(pwac, node);
		}
	}
}

static bool make_capture_sink(struct obs_pw_audio_capture_app *pwac, uint32_t channels, const char *position)
{
	/* HACK: In order to hide the app capture sink from PulseAudio applications, for example to prevent them from intentionally outputting
	 * to it, or to not fill up desktop audio control menus with sinks, the media class is set to Audio/Sink/Internal.
	 * This works because pipewire-pulse only reports nodes with the media class set to Audio/Sink as proper outputs
	 * https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/0.3.72/src/modules/module-protocol-pulse/manager.c?ref_type=tags#L944
	 * and because of https://gitlab.freedesktop.org/pipewire/pipewire/-/merge_requests/1564#note_1861698, which works the same for
	 * deciding what nodes need an audio adapter.
	 */
	struct pw_properties *sink_props =
		pw_properties_new(PW_KEY_NODE_NAME, "OBS", PW_KEY_NODE_DESCRIPTION, "OBS App Audio Capture Sink",
						  PW_KEY_FACTORY_NAME, "support.null-audio-sink", PW_KEY_MEDIA_CLASS, "Audio/Sink/Internal",
						  PW_KEY_NODE_VIRTUAL, "true", SPA_KEY_AUDIO_POSITION, position, NULL);

	pw_properties_setf(sink_props, PW_KEY_AUDIO_CHANNELS, "%u", channels);

	pwac->sink.proxy =
		pw_core_create_object(pwac->pw.core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &sink_props->dict, 0);

	obs_pw_audio_instance_sync(&pwac->pw);

	pw_properties_free(sink_props);

	if (!pwac->sink.proxy) {
		blog(LOG_WARNING, "[pipewire] Failed to create app capture sink");
		return false;
	}

	pwac->sink.channels = channels;
	dstr_copy(&pwac->sink.position, position);

	pwac->sink.id = SPA_ID_INVALID;
	pwac->sink.serial = SPA_ID_INVALID;

	pw_proxy_add_listener(pwac->sink.proxy, &pwac->sink.proxy_listener, &sink_proxy_events, pwac);

	while (pwac->sink.id == SPA_ID_INVALID || pwac->sink.serial == SPA_ID_INVALID || pwac->sink.ports.num != channels) {
		/* Iterate until the sink is bound and all the ports are registered */
		pw_loop_iterate(pw_thread_loop_get_loop(pwac->pw.thread_loop), -1);
	}

	if (pwac->sink.serial == 0) {
		pw_proxy_destroy(pwac->sink.proxy);
		return false;
	}

	blog(LOG_INFO, "[pipewire] Created app capture sink %u with %u channels and position %s", pwac->sink.id, channels,
		 position);

	connect_targets(pwac);

	pwac->sink.autoconnect_targets = true;

	if (obs_pw_audio_stream_connect(&pwac->pw.audio, pwac->sink.id, pwac->sink.serial, channels) < 0) {
		blog(LOG_WARNING, "[pipewire] Error connecting stream %p to app capture sink %u", pwac->pw.audio.stream,
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

	if (pw_stream_get_state(pwac->pw.audio.stream, NULL) != PW_STREAM_STATE_UNCONNECTED) {
		pw_stream_disconnect(pwac->pw.audio.stream);
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
	pw_proxy_destroy(pwac->default_sink.proxy);
}

static void on_default_sink_proxy_destroy_cb(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;
	spa_hook_remove(&pwac->default_sink.node_listener);
	spa_zero(pwac->default_sink.node_listener);

	spa_hook_remove(&pwac->default_sink.proxy_listener);
	spa_zero(pwac->default_sink.proxy_listener);

	pwac->default_sink.proxy = NULL;
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
	struct obs_pw_audio_proxy_list_iter iter;
	obs_pw_audio_proxy_list_iter_init(&iter, &pwac->system_sinks);

	struct system_sink *temp, *default_sink = NULL;
	while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&temp)) {
		if (strcmp(name, temp->name) == 0) {
			default_sink = temp;
			break;
		}
	}
	if (!default_sink) {
		return;
	}

	if (pwac->default_sink.proxy) {
		pw_proxy_destroy(pwac->default_sink.proxy);
	}

	pwac->default_sink.proxy =
		pw_registry_bind(pwac->pw.registry, default_sink->id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
	if (!pwac->default_sink.proxy) {
		if (!pwac->sink.proxy) {
			blog(LOG_WARNING, "[pipewire] Failed to get default sink info, app capture sink defaulting to stereo");
			make_capture_sink(pwac, 2, "FL,FR");
		}
		return;
	}

	pw_proxy_add_object_listener(pwac->default_sink.proxy, &pwac->default_sink.node_listener, &default_sink_events,
								 pwac);
	pw_proxy_add_listener(pwac->default_sink.proxy, &pwac->default_sink.proxy_listener, &default_sink_proxy_events,
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

	if (id == pwac->sink.id) {
		const char *ser = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
		if (!ser) {
			blog(LOG_ERROR, "[pipewire] No object serial found on app capture sink %u", id);
			pwac->sink.serial = 0;
		} else {
			pwac->sink.serial = strtoul(ser, NULL, 10);
		}
	}

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
			struct obs_pw_audio_proxy_list_iter iter;
			obs_pw_audio_proxy_list_iter_init(&iter, &pwac->nodes);

			struct target_node *temp, *node = NULL;
			while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&temp)) {
				if (temp->id == node_id) {
					node = temp;
					break;
				}
			}
			if (!node) {
				return;
			}

			struct target_node_port *port = node_register_port(node, id, pwac->pw.registry, chn);

			if (port && pwac->sink.autoconnect_targets && node_is_targeted(pwac, node)) {
				link_port_to_sink(pwac, port, node->id);
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

			uint32_t client_id = 0;
			const char *client_id_str = spa_dict_lookup(props, PW_KEY_CLIENT_ID);
			if (client_id_str) {
				client_id = strtoul(client_id_str, NULL, 10);
			}

			register_target_node(pwac, id, client_id, node_app_name, node_name);
		} else if (strcmp(media_class, "Audio/Sink") == 0) {
			register_system_sink(pwac, id, node_name);
		}

	} else if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
		const char *client_app_name = spa_dict_lookup(props, PW_KEY_APP_NAME);
		register_target_client(pwac, id, client_app_name);
	} else if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
		const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
		if (!name || strcmp(name, "default") != 0) {
			return;
		}

		if (!obs_pw_audio_default_node_metadata_listen(&pwac->default_sink.metadata, &pwac->pw, id, true,
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
static bool add_app_clicked(obs_properties_t *properties, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(properties);
	UNUSED_PARAMETER(property);

	obs_source_t *source = data;

	obs_data_t *settings = obs_source_get_settings(source);
	const char *app_to_add = obs_data_get_string(settings, SETTING_AVAILABLE_APPS);

	obs_data_array_t *selections = obs_data_get_array(settings, SETTING_SELECTION_MULTIPLE);

	/* Don't add if selection is already in the list */

	bool should_add = true;
	for (size_t i = 0; i < obs_data_array_count(selections) && should_add; i++) {
		obs_data_t *item = obs_data_array_item(selections, i);

		should_add = astrcmpi(obs_data_get_string(item, "value"), app_to_add) != 0;

		obs_data_release(item);
	}

	if (should_add) {
		obs_data_t *new_entry = obs_data_create();
		obs_data_set_bool(new_entry, "hidden", false);
		obs_data_set_bool(new_entry, "selected", false);
		obs_data_set_string(new_entry, "value", app_to_add);

		obs_data_array_push_back(selections, new_entry);

		obs_data_release(new_entry);

		obs_source_update(source, settings);
	}

	obs_data_array_release(selections);
	obs_data_release(settings);

	return should_add;
}

static int cmp_targets(const void *a, const void *b)
{
	const char *a_str = *(char **)a;
	const char *b_str = *(char **)b;
	return strcmp(a_str, b_str);
}

static const char *choose_display_string(struct obs_pw_audio_capture_app *pwac, const char *binary,
										 const char *app_name)
{
	switch (pwac->match_priority) {
	case MATCH_PRIORITY_BINARY_NAME:
		return binary ? binary : app_name;
	case MATCH_PRIORITY_APP_NAME:
		return app_name ? app_name : binary;
	}
}

static void populate_avaiable_apps_list(obs_property_t *list, struct obs_pw_audio_capture_app *pwac)
{
	DARRAY(const char *) targets;
	da_init(targets);

	pw_thread_loop_lock(pwac->pw.thread_loop);

	da_reserve(targets, pwac->n_nodes);

	struct obs_pw_audio_proxy_list_iter iter;
	obs_pw_audio_proxy_list_iter_init(&iter, &pwac->nodes);

	struct target_node *node;
	while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&node)) {
		const char *display = choose_display_string(pwac, node->binary, node->app_name);

		if (!display) {
			display = node->name;
		}

		da_push_back(targets, &display);
	}

	obs_pw_audio_proxy_list_iter_init(&iter, &pwac->clients);

	struct target_client *client;
	while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&client)) {
		const char *display = choose_display_string(pwac, client->binary, client->app_name);

		if (display) {
			da_push_back(targets, &display);
		}
	}

	/* Show just one entry per target */

	qsort(targets.array, targets.num, sizeof(const char *), cmp_targets);

	for (size_t i = 0; i < targets.num; i++) {
		if (i == 0 || strcmp(targets.array[i - 1], targets.array[i]) != 0) {
			obs_property_list_add_string(list, targets.array[i], targets.array[i]);
		}
	}

	pw_thread_loop_unlock(pwac->pw.thread_loop);

	da_free(targets);
}

static bool capture_mode_modified(void *data, obs_properties_t *properties, obs_property_t *property,
								  obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	struct obs_pw_audio_capture_app *pwac = data;

	enum capture_mode mode = obs_data_get_int(settings, SETTING_CAPTURE_MODE);

	switch (mode) {
	case CAPTURE_MODE_SINGLE: {
		obs_properties_remove_by_name(properties, SETTING_SELECTION_MULTIPLE);
		obs_properties_remove_by_name(properties, SETTING_AVAILABLE_APPS);
		obs_properties_remove_by_name(properties, SETTING_ADD_TO_SELECTIONS);

		obs_property_t *available_apps = obs_properties_add_list(properties, SETTING_SELECTION_SINGLE,
																 obs_module_text("Application"),
																 OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);

		populate_avaiable_apps_list(available_apps, pwac);

		break;
	}
	case CAPTURE_MODE_MULTIPLE: {
		obs_properties_remove_by_name(properties, SETTING_SELECTION_SINGLE);

		obs_properties_add_editable_list(properties, SETTING_SELECTION_MULTIPLE, obs_module_text("SelectedApps"),
										 OBS_EDITABLE_LIST_TYPE_STRINGS, NULL, NULL);

		obs_property_t *available_apps = obs_properties_add_list(properties, SETTING_AVAILABLE_APPS,
																 obs_module_text("Applications"), OBS_COMBO_TYPE_LIST,
																 OBS_COMBO_FORMAT_STRING);

		populate_avaiable_apps_list(available_apps, pwac);

		obs_properties_add_button2(properties, SETTING_ADD_TO_SELECTIONS, obs_module_text("AddToSelected"),
								   add_app_clicked, pwac->source);

		break;
	}
	}

	return true;
}

static bool match_priority_modified(void *data, obs_properties_t *properties, obs_property_t *property,
									obs_data_t *settings)
{
	UNUSED_PARAMETER(property);

	struct obs_pw_audio_capture_app *pwac = data;

	enum capture_mode mode = obs_data_get_int(settings, SETTING_CAPTURE_MODE);

	obs_property_t *targets = NULL;
	switch (mode) {
	default:
	case CAPTURE_MODE_SINGLE:
		targets = obs_properties_get(properties, SETTING_SELECTION_SINGLE);
		break;
	case CAPTURE_MODE_MULTIPLE:
		targets = obs_properties_get(properties, SETTING_AVAILABLE_APPS);
		break;
	}

	if (targets == NULL) {
		return false;
	}

	obs_property_list_clear(targets);

	populate_avaiable_apps_list(targets, pwac);

	return true;
}

static void build_selections(struct obs_pw_audio_capture_app *pwac, obs_data_t *settings)
{
	switch (pwac->capture_mode) {
	case CAPTURE_MODE_SINGLE: {
		const char *selection = bstrdup(obs_data_get_string(settings, SETTING_SELECTION_SINGLE));
		da_push_back(pwac->selections, &selection);
		break;
	}
	case CAPTURE_MODE_MULTIPLE: {
		obs_data_array_t *selections = obs_data_get_array(settings, SETTING_SELECTION_MULTIPLE);
		for (size_t i = 0; i < obs_data_array_count(selections); i++) {
			obs_data_t *item = obs_data_array_item(selections, i);

			const char *selection = bstrdup(obs_data_get_string(item, "value"));
			da_push_back(pwac->selections, &selection);

			obs_data_release(item);
		}
		obs_data_array_release(selections);
		break;
	}
	}
}

static void clear_selections(struct obs_pw_audio_capture_app *pwac)
{
	for (size_t i = 0; i < pwac->selections.num; i++) {
		const char *selection = pwac->selections.array[i];
		bfree((void *)selection);
	}

	da_clear(pwac->selections);
}

static void *pipewire_audio_capture_app_create(obs_data_t *settings, obs_source_t *source)
{
	struct obs_pw_audio_capture_app *pwac = bzalloc(sizeof(struct obs_pw_audio_capture_app));

	if (!obs_pw_audio_instance_init(&pwac->pw, &registry_events, pwac, true, false, source)) {
		obs_pw_audio_instance_destroy(&pwac->pw);

		bfree(pwac);
		return NULL;
	}

	pwac->source = source;

	obs_pw_audio_proxy_list_init(&pwac->nodes, NULL, node_destroy_cb);
	obs_pw_audio_proxy_list_init(&pwac->clients, NULL, client_destroy_cb);
	obs_pw_audio_proxy_list_init(&pwac->sink.links, link_bound_cb, link_destroy_cb);
	obs_pw_audio_proxy_list_init(&pwac->system_sinks, NULL, system_sink_destroy_cb);

	pwac->sink.id = SPA_ID_INVALID;
	dstr_init(&pwac->sink.position);

	pwac->capture_mode = obs_data_get_int(settings, SETTING_CAPTURE_MODE);
	pwac->match_priority = obs_data_get_int(settings, SETTING_MATCH_PRIORITY);
	pwac->except = obs_data_get_bool(settings, SETTING_EXCLUDE_SELECTIONS);

	da_init(pwac->selections);
	build_selections(pwac, settings);

	obs_pw_audio_instance_sync(&pwac->pw);
	pw_thread_loop_wait(pwac->pw.thread_loop);
	pw_thread_loop_unlock(pwac->pw.thread_loop);

	return pwac;
}

static void pipewire_audio_capture_app_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SETTING_CAPTURE_MODE, CAPTURE_MODE_SINGLE);
	obs_data_set_default_int(settings, SETTING_MATCH_PRIORITY, MATCH_PRIORITY_BINARY_NAME);
	obs_data_set_default_bool(settings, SETTING_EXCLUDE_SELECTIONS, false);

	obs_data_array_t *arr = obs_data_array_create();
	obs_data_set_default_array(settings, SETTING_SELECTION_MULTIPLE, arr);
	obs_data_array_release(arr);
}

static obs_properties_t *pipewire_audio_capture_app_properties(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;

	obs_properties_t *p = obs_properties_create();

	obs_property_t *capture_mode = obs_properties_add_list(p, SETTING_CAPTURE_MODE, obs_module_text("AppCaptureMode"),
														   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(capture_mode, obs_module_text("SingleApp"), CAPTURE_MODE_SINGLE);
	obs_property_list_add_int(capture_mode, obs_module_text("MultipleApps"), CAPTURE_MODE_MULTIPLE);
	obs_property_set_modified_callback2(capture_mode, capture_mode_modified, pwac);

	obs_property_t *match_priority = obs_properties_add_list(
		p, SETTING_MATCH_PRIORITY, obs_module_text("MatchPriority"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(match_priority, obs_module_text("MatchBinaryFirst"), MATCH_PRIORITY_BINARY_NAME);
	obs_property_list_add_int(match_priority, obs_module_text("MatchAppNameFirst"), MATCH_PRIORITY_APP_NAME);
	obs_property_set_modified_callback2(match_priority, match_priority_modified, pwac);

	obs_properties_add_bool(p, SETTING_EXCLUDE_SELECTIONS, obs_module_text("ExceptApp"));

	return p;
}

static void pipewire_audio_capture_app_update(void *data, obs_data_t *settings)
{
	struct obs_pw_audio_capture_app *pwac = data;

	pw_thread_loop_lock(pwac->pw.thread_loop);

	pwac->capture_mode = obs_data_get_int(settings, SETTING_CAPTURE_MODE);
	pwac->match_priority = obs_data_get_int(settings, SETTING_MATCH_PRIORITY);
	pwac->except = obs_data_get_bool(settings, SETTING_EXCLUDE_SELECTIONS);

	clear_selections(pwac);
	build_selections(pwac, settings);

	connect_targets(pwac);

	obs_pw_audio_instance_sync(&pwac->pw);
	pw_thread_loop_wait(pwac->pw.thread_loop);
	pw_thread_loop_unlock(pwac->pw.thread_loop);
}

static void pipewire_audio_capture_app_show(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;

	pw_thread_loop_lock(pwac->pw.thread_loop);
	pw_stream_set_active(pwac->pw.audio.stream, true);
	pw_thread_loop_unlock(pwac->pw.thread_loop);
}

static void pipewire_audio_capture_app_hide(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;

	pw_thread_loop_lock(pwac->pw.thread_loop);
	pw_stream_set_active(pwac->pw.audio.stream, false);
	pw_thread_loop_unlock(pwac->pw.thread_loop);
}

static void pipewire_audio_capture_app_destroy(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;

	pw_thread_loop_lock(pwac->pw.thread_loop);

	obs_pw_audio_proxy_list_clear(&pwac->nodes);
	obs_pw_audio_proxy_list_clear(&pwac->system_sinks);

	obs_pw_audio_proxy_list_clear(&pwac->clients);

	destroy_capture_sink(pwac);

	if (pwac->default_sink.proxy) {
		pw_proxy_destroy(pwac->default_sink.proxy);
	}
	if (pwac->default_sink.metadata.proxy) {
		pw_proxy_destroy(pwac->default_sink.metadata.proxy);
	}

	obs_pw_audio_instance_destroy(&pwac->pw);

	dstr_free(&pwac->sink.position);

	clear_selections(pwac);
	da_free(pwac->selections);

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
