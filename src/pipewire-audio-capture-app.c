/* pipewire-audio-capture-app.c
 *
 * Copyright 2022-2026 Dimitris Papaioannou <dimtpap@protonmail.com>
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

#include <stdio.h>
#include <string.h>

#include <pipewire/impl-module.h>
#include <spa/debug/types.h>

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
	uint32_t channels;
	struct obs_pw_audio_capture_app *pwac;
	struct obs_pw_audio_proxy_list ports;
	uint32_t *p_n_nodes;

	struct pw_proxy *proxy;
	struct spa_hook node_listener;

	/* Format / EnumFormat position, independent of the ports the current sink created. */
	struct dstr format_position;
	uint32_t format_channels;
	bool format_from_format;

	/* Sink this stream was playing to before it was moved. Empty follows the default. */
	struct dstr previous_sink;
	bool routed;
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

struct route_link {
	uint32_t id;
	uint32_t out_node;
	uint32_t in_node;
};

/** This source basically works like this:
    - Keep track of output streams, their Format position, system sinks and the default sink

    - Create one visible Audio/Sink. Size it from the targeted stream's Format position so
      the capture keeps that layout even if the default sink is stereo. Until a format
      arrives, fall back to the stream's ports and then the default sink layout.

    - Point the stream at that sink with default metadata. WirePlumber moves it.
      Play the sink back to the stream's previous output, or leave playback following
      the default when that was already the target.
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
		/* True once the sink layout came from targeted application ports. */
		bool from_targets;
		bool relayout;
		DARRAY(struct capture_sink_port) ports;

		/* Links between app streams and the capture sink */
		struct obs_pw_audio_proxy_list links;
	} sink;

	/** Default sink layout, used only until a targeted application has a format. */
	struct dstr default_position;
	uint32_t default_channels;
	struct dstr default_name;
	bool destroying;

	struct dstr sink_name;
	struct pw_impl_module *playback;
	struct spa_hook playback_listener;
	uint32_t playback_generation;
	bool playback_wanted;
	struct dstr playback_target;
	struct dstr playback_position;
	DARRAY(struct route_link) route_links;

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

static bool node_is_targeted(struct obs_pw_audio_capture_app *pwac, struct target_node *node);
static bool apply_target_layout(struct obs_pw_audio_capture_app *pwac);
static bool route_targets(struct obs_pw_audio_capture_app *pwac);
static void restore_stream_targets(struct obs_pw_audio_capture_app *pwac);
static void unload_playback(struct obs_pw_audio_capture_app *pwac);

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
	struct obs_pw_audio_capture_app *pwac = node->pwac;

	spa_hook_remove(&node->node_listener);

	obs_pw_audio_proxy_list_clear(&node->ports);

	(*node->p_n_nodes)--;

	bfree((void *)node->binary);
	bfree((void *)node->app_name);
	bfree((void *)node->name);
	dstr_free(&node->format_position);
	dstr_free(&node->previous_sink);

	/* The node is already removed from the list. Resize if this was the
	   stream that determined the capture layout. */
	if (pwac && !pwac->destroying)
		apply_target_layout(pwac);
}

static struct target_node_port *node_register_port(struct target_node *node, uint32_t global_id,
						   struct pw_registry *registry, const char *channel)
{
	struct pw_proxy *port_proxy = pw_registry_bind(registry, global_id, PW_TYPE_INTERFACE_Port, PW_VERSION_PORT,
						       sizeof(struct target_node_port));
	if (!port_proxy) {
		return NULL;
	}

	struct target_node_port *port = pw_proxy_get_user_data(port_proxy);
	port->channel = bstrdup(channel);
	port->id = global_id;

	obs_pw_audio_proxy_list_append(&node->ports, port_proxy);

	return port;
}

static const char *spa_channel_short_name(enum spa_audio_channel channel)
{
	static const struct {
		enum spa_audio_channel channel;
		const char *name;
	} table[] = {
		{SPA_AUDIO_CHANNEL_MONO, "MONO"}, {SPA_AUDIO_CHANNEL_FL, "FL"},
		{SPA_AUDIO_CHANNEL_FR, "FR"},     {SPA_AUDIO_CHANNEL_FC, "FC"},
		{SPA_AUDIO_CHANNEL_LFE, "LFE"},   {SPA_AUDIO_CHANNEL_SL, "SL"},
		{SPA_AUDIO_CHANNEL_SR, "SR"},     {SPA_AUDIO_CHANNEL_FLC, "FLC"},
		{SPA_AUDIO_CHANNEL_FRC, "FRC"},   {SPA_AUDIO_CHANNEL_RC, "RC"},
		{SPA_AUDIO_CHANNEL_RL, "RL"},     {SPA_AUDIO_CHANNEL_RR, "RR"},
		{SPA_AUDIO_CHANNEL_TC, "TC"},     {SPA_AUDIO_CHANNEL_TFL, "TFL"},
		{SPA_AUDIO_CHANNEL_TFC, "TFC"},   {SPA_AUDIO_CHANNEL_TFR, "TFR"},
		{SPA_AUDIO_CHANNEL_TRL, "TRL"},   {SPA_AUDIO_CHANNEL_TRC, "TRC"},
		{SPA_AUDIO_CHANNEL_TRR, "TRR"},   {SPA_AUDIO_CHANNEL_RLC, "RLC"},
		{SPA_AUDIO_CHANNEL_RRC, "RRC"},   {SPA_AUDIO_CHANNEL_FLW, "FLW"},
		{SPA_AUDIO_CHANNEL_FRW, "FRW"},   {SPA_AUDIO_CHANNEL_LFE2, "LFE2"},
		{SPA_AUDIO_CHANNEL_FLH, "FLH"},   {SPA_AUDIO_CHANNEL_FCH, "FCH"},
		{SPA_AUDIO_CHANNEL_FRH, "FRH"},   {SPA_AUDIO_CHANNEL_AUX0, "AUX0"},
		{SPA_AUDIO_CHANNEL_AUX1, "AUX1"}, {SPA_AUDIO_CHANNEL_AUX2, "AUX2"},
		{SPA_AUDIO_CHANNEL_AUX3, "AUX3"}, {SPA_AUDIO_CHANNEL_AUX4, "AUX4"},
		{SPA_AUDIO_CHANNEL_AUX5, "AUX5"}, {SPA_AUDIO_CHANNEL_AUX6, "AUX6"},
		{SPA_AUDIO_CHANNEL_AUX7, "AUX7"},
	};

	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (table[i].channel == channel)
			return table[i].name;
	}
	return NULL;
}

static void on_node_param_cb(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
			     const struct spa_pod *param)
{
	struct target_node *node = data;
	struct spa_audio_info_raw info;
	struct dstr position;
	uint32_t named = 0;

	UNUSED_PARAMETER(seq);
	UNUSED_PARAMETER(index);
	UNUSED_PARAMETER(next);

	if ((id != SPA_PARAM_Format && id != SPA_PARAM_EnumFormat) || !param)
		return;
	if (spa_format_audio_raw_parse(param, &info) < 0 || !info.channels || info.channels > SPA_AUDIO_MAX_CHANNELS)
		return;

	/* Keep the widest offered layout. A negotiated Format wins ties, because that is
	   the layout the stream is actually producing. */
	if (info.channels < node->format_channels)
		return;
	if (info.channels == node->format_channels && id == SPA_PARAM_EnumFormat && node->format_from_format)
		return;

	dstr_init(&position);
	for (uint32_t i = 0; i < info.channels; i++) {
		const char *name = spa_channel_short_name(info.position[i]);
		if (!name)
			continue;
		if (named)
			dstr_cat(&position, ",");
		dstr_cat(&position, name);
		named++;
	}
	if (!named) {
		dstr_free(&position);
		return;
	}

	if (node->format_position.array && strcmp(node->format_position.array, position.array) == 0 &&
	    node->format_channels == named && node->format_from_format == (id == SPA_PARAM_Format)) {
		dstr_free(&position);
		return;
	}

	dstr_copy(&node->format_position, position.array);
	node->format_channels = named;
	node->format_from_format = id == SPA_PARAM_Format;
	dstr_free(&position);

	if (node->pwac && node_is_targeted(node->pwac, node))
		apply_target_layout(node->pwac);
}

static void on_node_info_cb(void *data, const struct pw_node_info *info)
{
	struct target_node *node = data;

	if ((info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) && node->proxy) {
		for (uint32_t i = 0; i < info->n_params; i++) {
			uint32_t param_id = info->params[i].id;
			if (param_id != SPA_PARAM_Format && param_id != SPA_PARAM_EnumFormat)
				continue;
			if (info->params[i].flags & SPA_PARAM_INFO_READ)
				pw_node_enum_params((struct pw_node *)node->proxy, 0, param_id, 0, UINT32_MAX, NULL);
		}
	}

	if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) == 0 || !info->props || !info->props->n_items) {
		return;
	}

	const char *channels = spa_dict_lookup(info->props, PW_KEY_AUDIO_CHANNELS);
	if (channels) {
		uint32_t c = strtoul(channels, NULL, 10);
		if (c && c != node->channels) {
			node->channels = c;
			if (node->pwac && node_is_targeted(node->pwac, node))
				apply_target_layout(node->pwac);
		}
	}

	const char *binary = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_BINARY);
	if (!binary)
		return;

	bfree((void *)node->binary);
	node->binary = bstrdup(binary);

	if (node->pwac && node_is_targeted(node->pwac, node))
		apply_target_layout(node->pwac);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = on_node_info_cb,
	.param = on_node_param_cb,
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
	node->channels = 0;
	node->pwac = pwac;
	node->p_n_nodes = &pwac->n_nodes;
	node->proxy = node_proxy;
	dstr_init(&node->format_position);
	dstr_init(&node->previous_sink);
	obs_pw_audio_proxy_list_init(&node->ports, NULL, port_destroy_cb);

	pwac->n_nodes++;

	obs_pw_audio_proxy_list_append(&pwac->nodes, node_proxy);
	pw_proxy_add_object_listener(node_proxy, &node->node_listener, &node_events, node);
}

static bool node_is_targeted(struct obs_pw_audio_capture_app *pwac, struct target_node *node)
{
	bool targeted = false;

	/* The playback loopback is an output stream of this source. Never capture it,
	   including when the source is in exclude mode. */
	if (node->name && strncmp(node->name, "OBS:", 4) == 0)
		return false;

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
					targeted = (astrcmpi(selection, client->binary) == 0 ||
						    astrcmpi(selection, client->app_name) == 0);
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
	blog(LOG_DEBUG, "[pipewire-audio] Link %u destroyed", link->id);
}

static void link_port_to_sink(struct obs_pw_audio_capture_app *pwac, struct target_node_port *port, uint32_t node_id)
{
	blog(LOG_DEBUG, "[pipewire-audio] Connecting port %u of node %u to app capture sink", port->id, node_id);

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
		blog(LOG_WARNING,
		     "[pipewire-audio] Could not connect port %u of node %u to app capture sink. No port of app capture sink has channel %s",
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

	pw_properties_free(link_props);

	if (!link_proxy) {
		blog(LOG_WARNING, "[pipewire-audio] Could not connect port %u of node %u to app capture sink", port->id,
		     node_id);
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

static void finalize_capture_sink(struct obs_pw_audio_capture_app *pwac)
{
	if (!pwac->sink.proxy || pwac->sink.id == SPA_ID_INVALID || pwac->sink.serial == SPA_ID_INVALID ||
	    pwac->sink.ports.num != pwac->sink.channels) {
		return;
	}

	blog(LOG_DEBUG, "[pipewire-audio] App capture sink ready");

	pwac->sink.autoconnect_targets = true;

	blog(LOG_INFO, "[pipewire-audio] App capture sink %u using %u channels (%s)%s", pwac->sink.id,
	     pwac->sink.channels, pwac->sink.position.array, pwac->sink.from_targets ? " from application" : "");

	if (obs_pw_audio_stream_connect(&pwac->pw.audio, pwac->sink.id, pwac->sink.serial, pwac->sink.channels,
					pwac->sink.position.array) < 0) {
		blog(LOG_WARNING, "[pipewire-audio] Error connecting stream %p to app capture sink %u",
		     pwac->pw.audio.stream, pwac->sink.id);
	}

	/* WirePlumber links the application. Fall back to name-matched links if metadata is missing. */
	if (!route_targets(pwac))
		connect_targets(pwac);
}

static void on_sink_proxy_bound_cb(void *data, uint32_t global_id)
{
	struct obs_pw_audio_capture_app *pwac = data;
	pwac->sink.id = global_id;
	da_init(pwac->sink.ports);
}

static void on_sink_proxy_removed_cb(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;
	blog(LOG_WARNING, "[pipewire-audio] App capture sink %u has been destroyed by the PipeWire remote",
	     pwac->sink.id);
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

	blog(LOG_DEBUG, "[pipewire-audio] App capture sink %u destroyed", pwac->sink.id);

	pwac->sink.id = SPA_ID_INVALID;
}

static void on_sink_proxy_error_cb(void *data, int seq, int res, const char *message)
{
	UNUSED_PARAMETER(data);
	blog(LOG_ERROR, "[pipewire-audio] App capture sink error: seq:%d res:%d :%s", seq, res, message);
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
	blog(LOG_DEBUG, "[pipewire-audio] Registering app capture sink port %u", global_id);

	struct capture_sink_port *port = da_push_back_new(pwac->sink.ports);
	port->channel = bstrdup(channel);
	port->id = global_id;

	finalize_capture_sink(pwac);
}

static void make_capture_sink(struct obs_pw_audio_capture_app *pwac, uint32_t channels, const char *position)
{
	struct pw_properties *sink_props;
	char position_prop[160];
	size_t j = 0;

	if (pwac->destroying)
		return;

	/* PipeWire parses this as a relaxed JSON array. A comma-separated list
	   warns and a bare node name is not a channel map. */
	if (position && position[0] == '[') {
		snprintf(position_prop, sizeof(position_prop), "%s", position);
	} else {
		position_prop[j++] = '[';
		position_prop[j++] = ' ';
		for (size_t i = 0; position && position[i] && j + 3 < sizeof(position_prop); i++)
			position_prop[j++] = position[i] == ',' ? ' ' : position[i];
		position_prop[j++] = ' ';
		position_prop[j++] = ']';
		position_prop[j] = '\0';
	}

	sink_props =
		pw_properties_new(PW_KEY_FACTORY_NAME, "support.null-audio-sink", PW_KEY_MEDIA_CLASS, "Audio/Sink",
				  PW_KEY_NODE_VIRTUAL, "true", PW_KEY_PRIORITY_SESSION, "1", PW_KEY_PRIORITY_DRIVER,
				  "1", "node.autoconnect", "false", SPA_KEY_AUDIO_POSITION, position_prop, NULL);

	dstr_printf(&pwac->sink_name, "OBS: %s", obs_source_get_name(pwac->source));
	pw_properties_set(sink_props, PW_KEY_NODE_NAME, pwac->sink_name.array);

	pw_properties_setf(sink_props, PW_KEY_AUDIO_CHANNELS, "%u", channels);

	pwac->sink.proxy = pw_core_create_object(pwac->pw.core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
						 &sink_props->dict, 0);

	pw_properties_free(sink_props);

	if (!pwac->sink.proxy) {
		blog(LOG_WARNING, "[pipewire-audio] Failed to create app capture sink");
		return;
	}

	pwac->sink.channels = channels;
	dstr_copy(&pwac->sink.position, position);

	pwac->sink.id = SPA_ID_INVALID;
	pwac->sink.serial = SPA_ID_INVALID;

	pw_proxy_add_listener(pwac->sink.proxy, &pwac->sink.proxy_listener, &sink_proxy_events, pwac);

	blog(LOG_DEBUG, "[pipewire-audio] Created app capture sink");
}

static void destroy_capture_sink(struct obs_pw_audio_capture_app *pwac)
{
	/* Drop the loopback before the sink. A stream error makes the loopback
	   module destroy itself, and freeing it again aborts the process. */
	unload_playback(pwac);

	/* Links are automatically destroyed by PipeWire */

	if (!pwac->sink.proxy) {
		return;
	}

	if (pw_stream_get_state(pwac->pw.audio.stream, NULL) != PW_STREAM_STATE_UNCONNECTED) {
		pw_stream_disconnect(pwac->pw.audio.stream);
	}

	pwac->sink.autoconnect_targets = false;
	pw_proxy_destroy(pwac->sink.proxy);
}

/* Channel names sorted into the order OBS speaker layouts expect. */
static const char *capture_channel_order[] = {
	"MONO", "FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR", "RC", "FLC", "FRC", "RLC", "RRC",
	"TC",   "TFL", "TFC", "TFR", "TRL", "TRC", "TRR", "FLW", "FRW", "LFE2", "FLH", "FCH", "FRH",
	"AUX0", "AUX1", "AUX2", "AUX3", "AUX4", "AUX5", "AUX6", "AUX7", NULL,
};

#define MAX_CAPTURE_CHANNELS 8

static int capture_channel_rank(const char *name)
{
	for (int i = 0; capture_channel_order[i]; i++) {
		if (astrcmpi(name, capture_channel_order[i]) == 0)
			return i;
	}
	return 1000;
}

static const char *canonical_channel_name(const char *name)
{
	for (int i = 0; capture_channel_order[i]; i++) {
		if (astrcmpi(name, capture_channel_order[i]) == 0)
			return capture_channel_order[i];
	}
	return name;
}

static uint32_t node_port_count(struct target_node *node)
{
	uint32_t n = 0;
	struct obs_pw_audio_proxy_list_iter iter;
	struct target_node_port *port;

	obs_pw_audio_proxy_list_iter_init(&iter, &node->ports);
	while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&port))
		n++;
	return n;
}

static bool target_node_layout_ready(struct target_node *node)
{
	uint32_t n = node_port_count(node);
	if (!n)
		return false;
	/* Wait until every port named by audio.channels exists, so a 5.1
	   stream is not snapshotted as stereo while its ports are still appearing. */
	if (node->channels > 0 && n < node->channels)
		return false;
	return true;
}

struct layout_channel {
	const char *name;
	int rank;
};

static int cmp_layout_channel(const void *a, const void *b)
{
	const struct layout_channel *ca = a;
	const struct layout_channel *cb = b;
	if (ca->rank != cb->rank)
		return ca->rank - cb->rank;
	return strcmp(ca->name, cb->name);
}

static void layout_add_name(struct layout_channel *found, uint32_t *n, const char *channel)
{
	if (!channel || !*channel || *n >= 64)
		return;
	for (uint32_t i = 0; i < *n; i++) {
		if (astrcmpi(found[i].name, channel) == 0)
			return;
	}
	found[*n].name = canonical_channel_name(channel);
	found[*n].rank = capture_channel_rank(channel);
	(*n)++;
}

static void layout_add_position(struct layout_channel *found, uint32_t *n, const char *position)
{
	char tmp[256];
	char *cursor;
	char *comma;

	if (!position || !*position)
		return;
	strncpy(tmp, position, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	cursor = tmp;
	while (*cursor) {
		comma = strchr(cursor, ',');
		if (comma)
			*comma = '\0';
		layout_add_name(found, n, cursor);
		if (!comma)
			break;
		cursor = comma + 1;
	}
}

static bool layout_from_targets(struct obs_pw_audio_capture_app *pwac, struct dstr *position, uint32_t *channels)
{
	struct layout_channel found[64];
	uint32_t n = 0;
	bool any_format = false;
	bool any_ports = false;
	struct obs_pw_audio_proxy_list_iter iter;
	struct target_node *node;

	obs_pw_audio_proxy_list_iter_init(&iter, &pwac->nodes);
	while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&node)) {
		if (!node_is_targeted(pwac, node) || !node->format_channels || !node->format_position.array ||
		    !node->format_position.array[0])
			continue;
		layout_add_position(found, &n, node->format_position.array);
		any_format = true;
	}

	/* Ports are only the sink the stream is linked to now. Use them when no Format has arrived. */
	if (!any_format) {
		obs_pw_audio_proxy_list_iter_init(&iter, &pwac->nodes);
		while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&node)) {
			struct obs_pw_audio_proxy_list_iter ports;
			struct target_node_port *port;

			if (!node_is_targeted(pwac, node) || !target_node_layout_ready(node))
				continue;
			any_ports = true;
			obs_pw_audio_proxy_list_iter_init(&ports, &node->ports);
			while (obs_pw_audio_proxy_list_iter_next(&ports, (void **)&port))
				layout_add_name(found, &n, port->channel);
		}
	}

	if ((!any_format && !any_ports) || !n)
		return false;

	qsort(found, n, sizeof(found[0]), cmp_layout_channel);
	if (n > MAX_CAPTURE_CHANNELS)
		n = MAX_CAPTURE_CHANNELS;

	dstr_free(position);
	for (uint32_t i = 0; i < n; i++) {
		if (i)
			dstr_cat(position, ",");
		dstr_cat(position, found[i].name);
	}
	*channels = n;
	return true;
}

/* Returns true when the sink was recreated. The new sink links targets once
   its ports exist. Returns false when the caller should link new ports itself. */
static bool apply_target_layout(struct obs_pw_audio_capture_app *pwac)
{
	struct dstr position;
	uint32_t channels = 0;
	bool from_targets;

	if (pwac->destroying || pwac->sink.relayout)
		return false;

	dstr_init(&position);
	from_targets = layout_from_targets(pwac, &position, &channels);
	if (!from_targets) {
		/* The stream node can disappear while PipeWire applies target.object.
		   Keep the sink that was already sized from its format. Replacing it
		   with the default sink layout disconnects the playback loopback. */
		dstr_free(&position);
		return false;
	}

	if (pwac->sink.proxy && pwac->sink.channels == channels && !dstr_is_empty(&pwac->sink.position) &&
	    dstr_cmp(&pwac->sink.position, position.array) == 0) {
		pwac->sink.from_targets = from_targets;
		dstr_free(&position);
		return false;
	}

	pwac->sink.relayout = true;
	pwac->sink.from_targets = from_targets;
	blog(LOG_INFO, "[pipewire-audio] App capture layout %s (%u channels)%s", position.array, channels,
	     from_targets ? " from application" : " from default sink");
	destroy_capture_sink(pwac);
	make_capture_sink(pwac, channels, position.array);
	pwac->sink.relayout = false;
	dstr_free(&position);
	return pwac->sink.proxy != NULL;
}

struct playback_req {
	uint32_t generation;
};

static void commas_to_spaces(const char *in, char *out, size_t out_len)
{
	size_t j = 0;
	if (!in)
		in = "";
	for (size_t i = 0; in[i] && j + 1 < out_len; i++)
		out[j++] = in[i] == ',' ? ' ' : in[i];
	out[j] = '\0';
}

static void json_quote(const char *in, char *out, size_t out_len)
{
	size_t j = 0;
	if (!in)
		in = "";
	for (size_t i = 0; in[i] && j + 2 < out_len; i++) {
		if (in[i] == '\\' || in[i] == '"') {
			if (j + 3 >= out_len)
				break;
			out[j++] = '\\';
		}
		out[j++] = in[i];
	}
	out[j] = '\0';
}

static bool dstr_eq(const struct dstr *str, const char *value)
{
	const char *have = str->array ? str->array : "";
	return strcmp(have, value ? value : "") == 0;
}

static void on_playback_module_destroy(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;

	/* The loopback module destroys itself when one of its streams disconnects. */
	spa_hook_remove(&pwac->playback_listener);
	spa_zero(pwac->playback_listener);
	pwac->playback = NULL;
}

static const struct pw_impl_module_events playback_module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = on_playback_module_destroy,
};

static void unload_playback(struct obs_pw_audio_capture_app *pwac)
{
	struct pw_impl_module *module = pwac->playback;

	if (!module)
		return;
	/* The destroy hook clears pwac->playback. PipeWire also cancels a
	   self-destroy that was already queued for this module. */
	pw_impl_module_destroy(module);
}

static void load_playback(struct obs_pw_audio_capture_app *pwac)
{
	char position[128];
	char sink[256];
	char target[256];
	char target_prop[320];
	char args[2048];
	const char *pos = pwac->playback_position.array ? pwac->playback_position.array : "";
	int written;

	unload_playback(pwac);
	if (pwac->destroying || !pwac->sink.proxy || !pos[0] || dstr_is_empty(&pwac->sink_name))
		return;

	commas_to_spaces(pos, position, sizeof(position));
	json_quote(pwac->sink_name.array, sink, sizeof(sink));
	target_prop[0] = '\0';
	if (pwac->playback_target.array && pwac->playback_target.array[0]) {
		json_quote(pwac->playback_target.array, target, sizeof(target));
		snprintf(target_prop, sizeof(target_prop), " target.object = \"%s\"", target);
	}

	written = snprintf(args, sizeof(args),
			   "capture.props = { node.name = \"%s.monitor\" stream.capture.sink = true "
			   "target.object = \"%s\" node.passive = true stream.dont-remix = true "
			   "audio.position = [ %s ] } "
			   "playback.props = { node.name = \"%s.playback\" media.name = \"%s\" node.passive = true "
			   "channelmix.mix-lfe = true channelmix.normalize = false%s } "
			   "audio.position = [ %s ]",
			   sink, sink, position, sink, sink, target_prop, position);
	if (written < 0 || (size_t)written >= sizeof(args)) {
		blog(LOG_WARNING, "[pipewire-audio] Playback loopback arguments do not fit");
		return;
	}

	pwac->playback = pw_context_load_module(pwac->pw.context, "libpipewire-module-loopback", args, NULL);
	if (!pwac->playback) {
		blog(LOG_WARNING, "[pipewire-audio] Failed to load playback loopback for %s", pwac->sink_name.array);
		return;
	}
	pw_impl_module_add_listener(pwac->playback, &pwac->playback_listener, &playback_module_events, pwac);

	blog(LOG_INFO, "[pipewire-audio] Playing %s back to %s", pwac->sink_name.array,
	     (pwac->playback_target.array && pwac->playback_target.array[0]) ? pwac->playback_target.array
									     : "the default sink");
}

static int playback_invoke(struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size,
			   void *user_data)
{
	const struct playback_req *req = data;
	struct obs_pw_audio_capture_app *pwac = user_data;

	UNUSED_PARAMETER(loop);
	UNUSED_PARAMETER(async);
	UNUSED_PARAMETER(seq);

	if (!req || size < sizeof(*req) || pwac->destroying || req->generation != pwac->playback_generation)
		return 0;
	if (pwac->playback_wanted)
		load_playback(pwac);
	else
		unload_playback(pwac);
	return 0;
}

static int unload_playback_invoke(struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size,
				  void *user_data)
{
	UNUSED_PARAMETER(loop);
	UNUSED_PARAMETER(async);
	UNUSED_PARAMETER(seq);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
	unload_playback(user_data);
	return 0;
}

static void schedule_playback(struct obs_pw_audio_capture_app *pwac, bool load)
{
	struct playback_req req;

	pwac->playback_generation++;
	pwac->playback_wanted = load;
	req.generation = pwac->playback_generation;
	pw_loop_invoke(pw_thread_loop_get_loop(pwac->pw.thread_loop), playback_invoke, 0, &req, sizeof(req), false,
		       pwac);
}

static const char *system_sink_name_by_id(struct obs_pw_audio_capture_app *pwac, uint32_t id)
{
	struct obs_pw_audio_proxy_list_iter iter;
	struct system_sink *sink;

	obs_pw_audio_proxy_list_iter_init(&iter, &pwac->system_sinks);
	while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&sink)) {
		if (sink->id == id)
			return sink->name;
	}
	return NULL;
}

static const char *explicit_previous_sink(struct obs_pw_audio_capture_app *pwac, uint32_t node_id)
{
	for (size_t i = 0; i < pwac->route_links.num; i++) {
		const char *name;
		if (pwac->route_links.array[i].out_node != node_id)
			continue;
		name = system_sink_name_by_id(pwac, pwac->route_links.array[i].in_node);
		if (!name || dstr_eq(&pwac->sink_name, name))
			continue;
		if (dstr_eq(&pwac->default_name, name))
			continue;
		return name;
	}
	return NULL;
}

static bool route_targets(struct obs_pw_audio_capture_app *pwac)
{
	struct obs_pw_audio_proxy_list_iter iter;
	struct target_node *node;
	struct pw_metadata *metadata;
	const char *playback = NULL;
	bool any = false;

	if (pwac->destroying || !pwac->sink.proxy || pwac->sink.id == SPA_ID_INVALID || dstr_is_empty(&pwac->sink_name))
		return false;
	if (!pwac->default_sink.metadata.proxy)
		return false;

	metadata = (struct pw_metadata *)pwac->default_sink.metadata.proxy;
	obs_pw_audio_proxy_list_iter_init(&iter, &pwac->nodes);
	while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&node)) {
		const char *previous;
		int res;
		if (!node_is_targeted(pwac, node))
			continue;
		any = true;
		if (!node->routed) {
			previous = explicit_previous_sink(pwac, node->id);
			if (previous)
				dstr_copy(&node->previous_sink, previous);
		}
		res = pw_metadata_set_property(metadata, node->id, "target.object", "Spa:String", pwac->sink_name.array);
		if (res < 0) {
			blog(LOG_WARNING, "[pipewire-audio] Could not move %s to %s (%d)", node->name,
			     pwac->sink_name.array, res);
			link_node_to_sink(pwac, node);
			continue;
		}
		if (!node->routed) {
			uint32_t ports = node_port_count(node);
			node->routed = true;
			blog(LOG_INFO, "[pipewire-audio] Moved %s to %s", node->name, pwac->sink_name.array);
			if (node->format_channels && ports && ports < node->format_channels) {
				blog(LOG_INFO,
				     "[pipewire-audio] %s has %u ports but its format has %u channels. "
				     "Start this capture source before the application, or restart the application once, "
				     "if the capture stays short.",
				     node->name, ports, node->format_channels);
			}
		}
		/* An empty previous sink, or the default output, keeps playback untargeted
		   so it follows default-sink changes. */
		if (!playback && node->previous_sink.array && node->previous_sink.array[0] &&
		    !dstr_eq(&pwac->default_name, node->previous_sink.array))
			playback = node->previous_sink.array;
	}

	if (!any) {
		if (pwac->playback || pwac->playback_wanted)
			schedule_playback(pwac, false);
		return true;
	}

	if (pwac->playback && dstr_eq(&pwac->playback_target, playback) &&
	    dstr_eq(&pwac->playback_position, pwac->sink.position.array))
		return true;

	dstr_copy(&pwac->playback_target, playback ? playback : "");
	dstr_copy(&pwac->playback_position, pwac->sink.position.array ? pwac->sink.position.array : "");
	schedule_playback(pwac, true);
	return true;
}

static void restore_stream_targets(struct obs_pw_audio_capture_app *pwac)
{
	struct obs_pw_audio_proxy_list_iter iter;
	struct target_node *node;
	struct pw_metadata *metadata;

	if (!pwac->default_sink.metadata.proxy)
		return;

	metadata = (struct pw_metadata *)pwac->default_sink.metadata.proxy;
	obs_pw_audio_proxy_list_iter_init(&iter, &pwac->nodes);
	while (obs_pw_audio_proxy_list_iter_next(&iter, (void **)&node)) {
		const char *previous = NULL;
		if (!node->routed)
			continue;
		if (node->previous_sink.array && node->previous_sink.array[0] &&
		    !dstr_eq(&pwac->default_name, node->previous_sink.array))
			previous = node->previous_sink.array;
		if (previous)
			pw_metadata_set_property(metadata, node->id, "target.object", "Spa:String", previous);
		else
			pw_metadata_set_property(metadata, node->id, "target.object", NULL, NULL);
		node->routed = false;
		blog(LOG_INFO, "[pipewire-audio] Restored %s to %s", node->name ? node->name : "stream",
		     previous ? previous : "the default sink");
	}
}

/* ------------------------------------------------- */

/* Default system sink.
   EnumFormat is the layout PipeWire will actually negotiate. audio.channels
   can disagree with it. A targeted stream still wins once its own format arrives. */
static void on_default_sink_param_cb(void *data, int seq, uint32_t id, uint32_t index, uint32_t next,
				     const struct spa_pod *param)
{
	UNUSED_PARAMETER(seq);
	UNUSED_PARAMETER(index);
	UNUSED_PARAMETER(next);

	if (id != SPA_PARAM_EnumFormat) {
		return;
	}

	struct obs_pw_audio_capture_app *pwac = data;

	uint32_t media_type = 0, media_subtype = 0, parsed_id = 0, channels = 0;
	struct spa_pod *position_pod = NULL;

	struct spa_pod_parser p;
	spa_pod_parser_pod(&p, param);

	spa_pod_parser_get_object(&p, SPA_TYPE_OBJECT_Format, &parsed_id, SPA_FORMAT_mediaType, SPA_POD_Id(&media_type),
				  SPA_FORMAT_mediaSubtype, SPA_POD_Id(&media_subtype), SPA_FORMAT_AUDIO_channels,
				  SPA_POD_OPT_Int(&channels), SPA_FORMAT_AUDIO_position,
				  SPA_POD_OPT_Pod(&position_pod));

	if (pwac->sink.channels && !channels) {
		/* A later EnumFormat carried no channel count. Keep the one already chosen. */
		return;
	}

	if (parsed_id != SPA_PARAM_EnumFormat || media_type != SPA_MEDIA_TYPE_audio || !channels || !position_pod) {
		goto stereo_fallback;
	}

	uint32_t position_n = 0;
	uint32_t *position_arr = spa_pod_get_array(position_pod, &position_n);

	struct dstr position_str;
	dstr_init(&position_str);

	for (size_t i = 0; i < position_n; i++) {
		const char *chn = spa_debug_type_find_short_name(spa_type_audio_channel, position_arr[i]);

		if (strstr(chn, "AUX") != NULL) {
			/* Pro Audio sinks use AUX channels. Capture those as stereo. */
			channels = 2;
			dstr_copy(&position_str, "FL,FR");
			break;
		}

		dstr_cat(&position_str, chn);

		if (position_n - 1 != i) {
			dstr_cat_ch(&position_str, ',');
		}
	}

	dstr_copy(&pwac->default_position, position_str.array);
	pwac->default_channels = channels;

	/* A targeted application already determined the capture layout. */
	if (pwac->sink.from_targets) {
		dstr_free(&position_str);
		return;
	}

	if (channels != pwac->sink.channels || dstr_cmpi(&position_str, pwac->sink.position.array) != 0) {
		destroy_capture_sink(pwac);
		make_capture_sink(pwac, channels, position_str.array);
	}

	dstr_free(&position_str);
	return;

stereo_fallback:
	if (pwac->sink.proxy || pwac->sink.from_targets) {
		return;
	}

	blog(LOG_WARNING, "[pipewire-audio] Could not parse format of default sink. Falling back to stereo.");

	dstr_copy(&pwac->default_position, "FL,FR");
	pwac->default_channels = 2;
	destroy_capture_sink(pwac);
	make_capture_sink(pwac, 2, "FL,FR");
}

static const struct pw_node_events default_sink_events = {
	PW_VERSION_NODE_EVENTS,
	.param = on_default_sink_param_cb,
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

	blog(LOG_DEBUG, "[pipewire-audio] New default sink %s", name);
	dstr_copy(&pwac->default_name, name);

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
			blog(LOG_WARNING,
			     "[pipewire-audio] Failed to get default sink info, app capture sink defaulting to stereo");
			make_capture_sink(pwac, 2, "FL,FR");
		}
		return;
	}

	pw_proxy_add_object_listener(pwac->default_sink.proxy, &pwac->default_sink.node_listener, &default_sink_events,
				     pwac);
	pw_proxy_add_listener(pwac->default_sink.proxy, &pwac->default_sink.proxy_listener, &default_sink_proxy_events,
			      pwac);

	pw_node_subscribe_params((struct pw_node *)pwac->default_sink.proxy, (uint32_t[]){SPA_PARAM_EnumFormat}, 1);
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
			blog(LOG_ERROR, "[pipewire-audio] No object serial found on app capture sink %u", id);
			pwac->sink.serial = 0;
		} else {
			pwac->sink.serial = strtoul(ser, NULL, 10);
			finalize_capture_sink(pwac);
		}
	}

	if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
		const char *nid, *dir, *chn;
		if (!(nid = spa_dict_lookup(props, PW_KEY_NODE_ID)) ||
		    !(dir = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)) ||
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

			if (port && node_is_targeted(pwac, node)) {
				/* Recreate the sink when this port completes a source layout.
				   Metadata moves the stream; link by hand only if that is unavailable. */
				if (!apply_target_layout(pwac) && pwac->sink.autoconnect_targets && !route_targets(pwac))
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
			blog(LOG_WARNING,
			     "[pipewire-audio] Failed to get default metadata, app capture sink defaulting to stereo");
			make_capture_sink(pwac, 2, "FL,FR");
		}
	} else if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
		const char *out_node = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
		const char *in_node = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
		struct route_link *link;

		if (!out_node || !in_node)
			return;
		link = da_push_back_new(pwac->route_links);
		link->id = id;
		link->out_node = strtoul(out_node, NULL, 10);
		link->in_node = strtoul(in_node, NULL, 10);
	}
}

static void on_global_remove_cb(void *data, uint32_t id)
{
	struct obs_pw_audio_capture_app *pwac = data;

	for (size_t i = 0; i < pwac->route_links.num; i++) {
		if (pwac->route_links.array[i].id == id) {
			da_erase(pwac->route_links, i);
			break;
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
static bool add_app_clicked(obs_properties_t *properties, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(properties);
	UNUSED_PARAMETER(property);

	obs_source_t *source = data;

	obs_data_t *settings = obs_source_get_settings(source);
	const char *app_to_add = obs_data_get_string(settings, SETTING_AVAILABLE_APPS);

	obs_data_array_t *selections = obs_data_get_array(settings, SETTING_SELECTION_MULTIPLE);
	if (obs_data_array_count(selections) == 0) {
		obs_data_array_release(selections);

		selections = obs_data_array_create();
		obs_data_set_array(settings, SETTING_SELECTION_MULTIPLE, selections);
	}

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
	default:
		return NULL;
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

		obs_property_t *available_apps =
			obs_properties_add_list(properties, SETTING_SELECTION_SINGLE, obs_module_text("Application"),
						OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);

		populate_avaiable_apps_list(available_apps, pwac);

		break;
	}
	case CAPTURE_MODE_MULTIPLE: {
		obs_properties_remove_by_name(properties, SETTING_SELECTION_SINGLE);

		obs_properties_add_editable_list(properties, SETTING_SELECTION_MULTIPLE,
						 obs_module_text("SelectedApps"), OBS_EDITABLE_LIST_TYPE_STRINGS, NULL,
						 NULL);

		obs_property_t *available_apps = obs_properties_add_list(properties, SETTING_AVAILABLE_APPS,
									 obs_module_text("Applications"),
									 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

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

	pwac->selections.num = 0;
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
	dstr_init(&pwac->default_position);
	dstr_init(&pwac->default_name);
	dstr_init(&pwac->sink_name);
	dstr_init(&pwac->playback_target);
	dstr_init(&pwac->playback_position);
	da_init(pwac->route_links);

	pwac->capture_mode = obs_data_get_int(settings, SETTING_CAPTURE_MODE);
	pwac->match_priority = obs_data_get_int(settings, SETTING_MATCH_PRIORITY);
	pwac->except = obs_data_get_bool(settings, SETTING_EXCLUDE_SELECTIONS);

	da_init(pwac->selections);
	build_selections(pwac, settings);

	/* The sink uses "OBS: <source>". The recording stream needs a different node
	   name so WirePlumber's target.object lookup cannot pick it instead of the sink. */
	{
		struct dstr capture_name;
		struct pw_properties *stream_props;

		dstr_init(&capture_name);
		dstr_printf(&capture_name, "OBS: %s.capture", obs_source_get_name(source));
		stream_props = pw_properties_new(PW_KEY_NODE_NAME, capture_name.array, NULL);
		pw_stream_update_properties(pwac->pw.audio.stream, &stream_props->dict);
		pw_properties_free(stream_props);
		dstr_free(&capture_name);
	}

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

	obs_property_t *capture_mode = obs_properties_add_list(
		p, SETTING_CAPTURE_MODE, obs_module_text("AppCaptureMode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
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

	if (!apply_target_layout(pwac) && pwac->sink.autoconnect_targets && !route_targets(pwac))
		connect_targets(pwac);

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
	struct pw_loop *loop = pw_thread_loop_get_loop(pwac->pw.thread_loop);

	pw_thread_loop_lock(pwac->pw.thread_loop);

	pwac->destroying = true;
	pwac->playback_generation++;
	restore_stream_targets(pwac);

	/* Let the thread loop unload the module. It cannot run while this lock is held. */
	pw_thread_loop_unlock(pwac->pw.thread_loop);
	pw_loop_invoke(loop, unload_playback_invoke, 0, NULL, 0, true, pwac);
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
	dstr_free(&pwac->default_position);
	dstr_free(&pwac->default_name);
	dstr_free(&pwac->sink_name);
	dstr_free(&pwac->playback_target);
	dstr_free(&pwac->playback_position);
	da_free(pwac->route_links);

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
