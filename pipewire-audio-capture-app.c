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

#include <obs-module.h>
#include <util/dstr.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>

#include "pipewire-audio.h"

/** Source for capturing applciation audio using PipeWire */

struct target_node_port {
	const char *channel;
	uint32_t id;

	struct obs_pw_audio_proxied_object obj;
};

struct target_node {
	const char *friendly_name;
	const char *name;
	uint32_t id;
	struct spa_list ports;

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

		/** Links between app streams and the capture sink */
		struct spa_list links;
	} sink;

	/** Need the default system sink to create
	  * the app capture sink with the same audio channels */
	struct spa_list system_sinks;
	struct {
		struct obs_pw_audio_metadata metadata;
		struct pw_proxy *sink;
		struct spa_hook sink_listener;
		struct spa_hook sink_proxy_listener;
	} default_info;

	struct spa_list targets;

	struct dstr target_name;
	bool except_app;
};

/** System sinks */
static void system_sink_destroy_cb(void *data)
{
	struct system_sink *s = data;
	bfree((void *)s->name);
}

static void register_system_sink(struct obs_pw_audio_capture_app *pwac,
				 const char *name, uint32_t global_id)
{
	struct pw_proxy *sink_proxy =
		pw_registry_bind(pwac->pw.registry, global_id,
				 PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
	if (!sink_proxy) {
		return;
	}

	struct system_sink *s = bmalloc(sizeof(struct system_sink));
	s->name = bstrdup(name);
	s->id = global_id;

	obs_pw_audio_proxied_object_init(&s->obj, sink_proxy,
					 &pwac->system_sinks, NULL,
					 system_sink_destroy_cb, s);
}
/* ------------------------------------------------- */

/** Target nodes and ports */
static void port_destroy_cb(void *data)
{
	struct target_node_port *p = data;
	bfree((void *)p->channel);
}

static void node_destroy_cb(void *data)
{
	struct target_node *n = data;

	struct target_node_port *p, *tp;
	spa_list_for_each_safe(p, tp, &n->ports, obj.link)
	{
		pw_proxy_destroy(p->obj.proxy);
	}

	bfree((void *)n->friendly_name);
	bfree((void *)n->name);
}

static struct target_node_port *node_register_port(struct target_node *node,
						   struct pw_registry *registry,
						   uint32_t global_id,
						   const char *channel)
{
	struct pw_proxy *port_proxy = pw_registry_bind(registry, global_id,
						       PW_TYPE_INTERFACE_Port,
						       PW_VERSION_PORT, 0);
	if (!port_proxy) {
		return NULL;
	}

	struct target_node_port *p = bmalloc(sizeof(struct target_node_port));
	p->channel = bstrdup(channel);
	p->id = global_id;

	obs_pw_audio_proxied_object_init(&p->obj, port_proxy, &node->ports,
					 NULL, port_destroy_cb, p);

	return p;
}

static void register_target_node(struct obs_pw_audio_capture_app *pwac,
				 const char *friendly_name, const char *name,
				 uint32_t global_id)
{
	struct pw_proxy *node_proxy =
		pw_registry_bind(pwac->pw.registry, global_id,
				 PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
	if (!node_proxy) {
		return;
	}

	struct target_node *n = bmalloc(sizeof(struct target_node));
	n->friendly_name = bstrdup(friendly_name);
	n->name = bstrdup(name);
	n->id = global_id;
	spa_list_init(&n->ports);

	obs_pw_audio_proxied_object_init(&n->obj, node_proxy, &pwac->targets,
					 NULL, node_destroy_cb, n);
}
/* ------------------------------------------------- */

/** App streams <-> Capture sink links */
static void link_bound_cb(void *data, uint32_t global_id)
{
	struct capture_sink_link *l = data;
	l->id = global_id;
}

static void link_destroy_cb(void *data)
{
	struct capture_sink_link *l = data;

	blog(LOG_DEBUG, "[pipewire] Link %u destroyed", l->id);
}

static void link_port_to_sink(struct obs_pw_audio_capture_app *pwac,
			      struct target_node_port *port, uint32_t node_id)
{
	blog(LOG_DEBUG,
	     "[pipewire] Connecting port %u of node %u to app capture sink",
	     port->id, node_id);

	uint32_t p = 0;
	if (pwac->sink.channels == 1 && /** Mono capture sink */
	    pwac->sink.ports.num >= 1) {
		p = pwac->sink.ports.array[0].id;
	} else {
		for (size_t i = 0; i < pwac->sink.ports.num; i++) {
			if (astrcmpi(pwac->sink.ports.array[i].channel,
				     port->channel) == 0) {
				p = pwac->sink.ports.array[i].id;
				break;
			}
		}
	}

	if (!p) {
		blog(LOG_WARNING,
		     "[pipewire] Could not connect port %u of node %u to app capture sink. No port of app capture sink has channel %s",
		     port->id, node_id, port->channel);
		return;
	}

	struct pw_properties *link_props =
		pw_properties_new(PW_KEY_OBJECT_LINGER, "false",
				  PW_KEY_FACTORY_NAME, "link-factory", NULL);

	pw_properties_setf(link_props, PW_KEY_LINK_OUTPUT_NODE, "%u", node_id);
	pw_properties_setf(link_props, PW_KEY_LINK_OUTPUT_PORT, "%u", port->id);

	pw_properties_setf(link_props, PW_KEY_LINK_INPUT_NODE, "%u",
			   pwac->sink.id);
	pw_properties_setf(link_props, PW_KEY_LINK_INPUT_PORT, "%u", p);

	struct pw_proxy *link_proxy = pw_core_create_object(
		pwac->pw.core, "link-factory", PW_TYPE_INTERFACE_Link,
		PW_VERSION_LINK, &link_props->dict, 0);

	pw_properties_free(link_props);

	if (!link_proxy) {
		blog(LOG_WARNING,
		     "[pipewire] Could not connect port %u of node %u to app capture sink",
		     port->id, node_id);
		return;
	}

	struct capture_sink_link *l = bmalloc(sizeof(struct capture_sink_link));
	l->id = SPA_ID_INVALID;

	obs_pw_audio_proxied_object_init(&l->obj, link_proxy, &pwac->sink.links,
					 link_bound_cb, link_destroy_cb, l);

	obs_pw_audio_instance_sync(&pwac->pw);
}

static void link_node_to_sink(struct obs_pw_audio_capture_app *pwac,
			      struct target_node *node)
{
	struct target_node_port *p;
	spa_list_for_each(p, &node->ports, obj.link)
	{
		link_port_to_sink(pwac, p, node->id);
	}
}
/* ------------------------------------------------- */

/** App capture sink */

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
	blog(LOG_WARNING,
	     "[pipewire] App capture sink %u has been destroyed by the PipeWire remote",
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

	blog(LOG_DEBUG, "[pipewire] App capture sink %u destroyed",
	     pwac->sink.id);

	pwac->sink.id = SPA_ID_INVALID;
}

static void on_sink_proxy_error_cb(void *data, int seq, int res,
				   const char *message)
{
	blog(LOG_ERROR, "[pipewire] App capture sink error: seq:%d res:%d :%s",
	     seq, res, message);
}

struct pw_proxy_events sink_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.bound = on_sink_proxy_bound_cb,
	.removed = on_sink_proxy_removed_cb,
	.destroy = on_sink_proxy_destroy_cb,
	.error = on_sink_proxy_error_cb,
};

static void register_capture_sink_port(struct obs_pw_audio_capture_app *pwac,
				       const char *channel, uint32_t global_id)
{
	struct capture_sink_port *p = da_push_back_new(pwac->sink.ports);
	p->channel = bstrdup(channel);
	p->id = global_id;
}

static void destroy_sink_links(struct obs_pw_audio_capture_app *pwac)
{
	struct capture_sink_link *l, *t;
	spa_list_for_each_safe(l, t, &pwac->sink.links, obj.link)
	{
		pw_proxy_destroy(l->obj.proxy);
	}
}

static void connect_targets(struct obs_pw_audio_capture_app *pwac)
{
	if (!pwac->sink.proxy) {
		return;
	}

	destroy_sink_links(pwac);

	if (dstr_is_empty(&pwac->target_name)) {
		return;
	}

	struct target_node *n;
	spa_list_for_each(n, &pwac->targets, obj.link)
	{
		if (dstr_cmp(&pwac->target_name, n->name) == 0 ^
		    pwac->except_app) {
			link_node_to_sink(pwac, n);
		}
	}
}

static bool make_capture_sink(struct obs_pw_audio_capture_app *pwac,
			      uint32_t channels, const char *position)
{
	struct pw_properties *sink_props = pw_properties_new(
		PW_KEY_NODE_NAME, "OBS", PW_KEY_NODE_DESCRIPTION,
		"OBS App Audio Capture Sink", PW_KEY_FACTORY_NAME,
		"support.null-audio-sink", PW_KEY_MEDIA_CLASS,
		"Audio/Sink/Virtual", PW_KEY_NODE_VIRTUAL, "true",
		SPA_KEY_AUDIO_POSITION, position, NULL);

	pw_properties_setf(sink_props, PW_KEY_AUDIO_CHANNELS, "%u", channels);

	pwac->sink.proxy = pw_core_create_object(pwac->pw.core, "adapter",
						 PW_TYPE_INTERFACE_Node,
						 PW_VERSION_NODE,
						 &sink_props->dict, 0);

	pw_properties_free(sink_props);

	if (!pwac->sink.proxy) {
		blog(LOG_WARNING,
		     "[pipewire] Failed to create app capture sink");
		return false;
	}

	pwac->sink.channels = channels;
	dstr_copy(&pwac->sink.position, position);

	pwac->sink.id = SPA_ID_INVALID;

	pw_proxy_add_listener(pwac->sink.proxy, &pwac->sink.proxy_listener,
			      &sink_proxy_events, pwac);

	obs_pw_audio_instance_sync(&pwac->pw);

	while (pwac->sink.id == SPA_ID_INVALID ||
	       pwac->sink.ports.num != channels) {
		/** Iterate until the sink is bound and all the ports are registered */
		pw_loop_iterate(pw_thread_loop_get_loop(pwac->pw.thread_loop),
				-1);
	}

	blog(LOG_INFO,
	     "[pipewire] Created app capture sink %u with %u channels and position %s",
	     pwac->sink.id, channels, position);

	connect_targets(pwac);

	pwac->sink.autoconnect_targets = true;

	if (!pwac->audio.stream) {
		return true;
	}

	if (obs_pw_audio_stream_connect(
		    &pwac->audio, PW_DIRECTION_INPUT, pwac->sink.id,
		    PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_AUTOCONNECT |
			    PW_STREAM_FLAG_DONT_RECONNECT,
		    channels) < 0) {
		blog(LOG_WARNING,
		     "[pipewire] Error connecting stream %p to app capture sink %u",
		     pwac->audio.stream, pwac->sink.id);
	}

	return true;
}

static void destroy_capture_sink(struct obs_pw_audio_capture_app *pwac)
{
	/** Links are automatically destroyed by PipeWire */

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
	struct obs_pw_audio_capture_app *pwac = data;

	if (!info->props || !info->props->n_items) {
		return;
	}

	const char *channels, *position;
	if (!(channels = spa_dict_lookup(info->props, PW_KEY_AUDIO_CHANNELS)) ||
	    !(position =
		      spa_dict_lookup(info->props, SPA_KEY_AUDIO_POSITION))) {
		/** Fallback to stereo if there's no info and no capture sink */
		if (pwac->sink.proxy) {
			return;
		}
		channels = "2";
		position = "FL,FR";
	}

	uint32_t c = atoi(channels);
	if (!c) {
		return;
	}

	/** No need to create a new capture sink if the channels are the same */
	if (pwac->sink.channels == c && !dstr_is_empty(&pwac->sink.position) &&
	    dstr_cmp(&pwac->sink.position, position) == 0) {
		return;
	}

	if (pwac->sink.proxy) {
		destroy_capture_sink(pwac);
	}
	make_capture_sink(pwac, c, position);
}

struct pw_node_events default_sink_events = {
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

static struct pw_proxy_events default_sink_proxy_events = {
	PW_VERSION_NODE_EVENTS,
	.removed = on_default_sink_proxy_removed_cb,
	.destroy = on_default_sink_proxy_destroy_cb,
};

static int on_metadata_property_cb(void *data, uint32_t id, const char *key,
				   const char *type, const char *value)
{
	UNUSED_PARAMETER(type);

	struct obs_pw_audio_capture_app *pwac = data;

	if (id == PW_ID_CORE && key && value &&
	    strcmp(key, "default.audio.sink") == 0) {

		char val[128];
		if (!json_object_find(value, "name", val, sizeof(val))) {
			return 0;
		}

		blog(LOG_DEBUG, "[pipewire] New default sink %s", val);

		/** Find the new default sink and bind to it to get its channel info */
		struct system_sink *t, *s = NULL;
		spa_list_for_each(t, &pwac->system_sinks, obj.link)
		{
			if (strcmp(val, t->name) == 0) {
				s = t;
				break;
			}
		}
		if (!s) {
			return 0;
		}

		if (pwac->default_info.sink) {
			pw_proxy_destroy(pwac->default_info.sink);
		}

		pwac->default_info.sink = pw_registry_bind(
			pwac->pw.registry, s->id, PW_TYPE_INTERFACE_Node,
			PW_VERSION_NODE, 0);
		if (!pwac->default_info.sink) {
			if (!pwac->sink.proxy) {
				blog(LOG_WARNING,
				     "[pipewire] Failed to get default sink info, app capture sink defaulting to stereo");
				make_capture_sink(pwac, 2, "FL,FR");
			}
			return 0;
		}

		pw_proxy_add_object_listener(pwac->default_info.sink,
					     &pwac->default_info.sink_listener,
					     &default_sink_events, pwac);
		pw_proxy_add_listener(pwac->default_info.sink,
				      &pwac->default_info.sink_proxy_listener,
				      &default_sink_proxy_events, pwac);
	}

	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = on_metadata_property_cb,
};
/* ------------------------------------------------- */

/* Registry */
static void on_global_cb(void *data, uint32_t id, uint32_t permissions,
			 const char *type, uint32_t version,
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
		if (!(nid = spa_dict_lookup(props, PW_KEY_NODE_ID)) ||
		    !(dir = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)) ||
		    !(chn = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL))) {
			return;
		}

		uint32_t node_id = atoi(nid);

		if (astrcmpi(dir, "in") == 0 &&
		    node_id == pwac->sink.id) { /** Capture sink port */
			register_capture_sink_port(pwac, chn, id);
		} else if (astrcmpi(dir, "out") ==
			   0) { /** Possibly a target port */
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

			struct target_node_port *p = node_register_port(
				n, pwac->pw.registry, id, chn);

			/** Connect new port to capture sink if the node is targeted */
			if (p && pwac->sink.autoconnect_targets &&
			    !dstr_is_empty(&pwac->target_name) &&
			    (dstr_cmp(&pwac->target_name, n->name) == 0 ^
			     pwac->except_app)) {
				link_port_to_sink(pwac, p, n->id);
			}
		}
	} else if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
		const char *node_name, *media_class;
		if (!(node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME)) ||
		    !(media_class =
			      spa_dict_lookup(props, PW_KEY_MEDIA_CLASS))) {
			return;
		}

		if (strcmp(media_class, "Stream/Output/Audio") == 0) {
			/** Target node */
			const char *node_friendly_name =
				spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);

			if (!node_friendly_name) {
				node_friendly_name = node_name;
			}

			register_target_node(pwac, node_friendly_name,
					     node_name, id);
		} else if (strcmp(media_class, "Audio/Sink") == 0) {
			/** Track system sinks to get info for the app capture sink */
			register_system_sink(pwac, node_name, id);
		}
	} else if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
		const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
		if (!name || strcmp(name, "default") != 0) {
			return;
		}

		if (!obs_pw_audio_metadata_listen(&pwac->default_info.metadata,
						  &pwac->pw, id,
						  &metadata_events, pwac) &&
		    !pwac->sink.proxy) {
			blog(LOG_WARNING,
			     "[pipewire] Failed to get default metadata, app capture sink defaulting to stereo");
			make_capture_sink(pwac, 2, "FL,FR");
		}
	}
}

const struct pw_registry_events registry_events_app = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global_cb,
};
/* ------------------------------------------------- */

/* Source */
static void *pipewire_audio_capture_app_create(obs_data_t *settings,
					       obs_source_t *source)
{
	struct obs_pw_audio_capture_app *pwac =
		bzalloc(sizeof(struct obs_pw_audio_capture_app));

	if (!obs_pw_audio_instance_init(&pwac->pw)) {
		pw_thread_loop_lock(pwac->pw.thread_loop);
		obs_pw_audio_instance_destroy(&pwac->pw);

		bfree(pwac);
		return NULL;
	}

	spa_list_init(&pwac->targets);
	spa_list_init(&pwac->sink.links);
	spa_list_init(&pwac->system_sinks);

	pwac->sink.id = SPA_ID_INVALID;
	dstr_init(&pwac->sink.position);

	dstr_init_copy(&pwac->target_name,
		       obs_data_get_string(settings, "TargetName"));
	pwac->except_app = obs_data_get_bool(settings, "ExceptApp");

	pw_thread_loop_lock(pwac->pw.thread_loop);

	pw_registry_add_listener(pwac->pw.registry, &pwac->pw.registry_listener,
				 &registry_events_app, pwac);

	struct pw_properties *props = obs_pw_audio_stream_properties();
	pw_properties_set(props, PW_KEY_NODE_ALWAYS_PROCESS, "true");
	if (obs_pw_audio_stream_init(&pwac->audio, &pwac->pw, props, source)) {
		blog(LOG_INFO, "[pipewire] Created stream %p",
		     pwac->audio.stream);
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
	obs_data_set_bool(settings, "ExceptApp", false);
}

static obs_properties_t *pipewire_audio_capture_app_properties(void *data)
{
	struct obs_pw_audio_capture_app *pwac = data;

	obs_properties_t *p = obs_properties_create();

	if (!pwac->pw.thread_loop || !pwac->pw.core) {
		return p;
	}

	obs_property_t *targets_list = obs_properties_add_list(
		p, "TargetName", obs_module_text("Applications"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	pw_thread_loop_lock(pwac->pw.thread_loop);

	struct target_node *n;
	spa_list_for_each(n, &pwac->targets, obj.link)
	{
		obs_property_list_add_string(targets_list, n->friendly_name,
					     n->name);
	}

	obs_properties_add_bool(p, "ExceptApp", obs_module_text("ExceptApp"));

	pw_thread_loop_unlock(pwac->pw.thread_loop);

	return p;
}

static void pipewire_audio_capture_app_update(void *data, obs_data_t *settings)
{
	struct obs_pw_audio_capture_app *pwac = data;

	if (!pwac->pw.thread_loop || !pwac->pw.core) {
		return;
	}

	pw_thread_loop_lock(pwac->pw.thread_loop);

	bool except = obs_data_get_bool(settings, "ExceptApp");

	const char *new_target_name =
		obs_data_get_string(settings, "TargetName");

	if (except == pwac->except_app &&
	    (!new_target_name || !*new_target_name ||
	     (!dstr_is_empty(&pwac->target_name) &&
	      dstr_cmp(&pwac->target_name, new_target_name) == 0))) {
		goto unlock;
	}

	pwac->except_app = except;

	if (new_target_name && *new_target_name) {
		dstr_copy(&pwac->target_name, new_target_name);
	}

	connect_targets(pwac);

	obs_pw_audio_instance_sync(&pwac->pw);
	pw_thread_loop_wait(pwac->pw.thread_loop);

unlock:
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

	if (pwac->pw.thread_loop) {
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

		if (pwac->sink.proxy) {
			destroy_capture_sink(pwac);
		}

		if (pwac->default_info.sink) {
			pw_proxy_destroy(pwac->default_info.sink);
		}
		if (pwac->default_info.metadata.proxy) {
			pw_proxy_destroy(pwac->default_info.metadata.proxy);
		}

		obs_pw_audio_instance_destroy(&pwac->pw);
	}

	dstr_free(&pwac->sink.position);
	dstr_free(&pwac->target_name);

	bfree(pwac);
}

const char *pipewire_audio_capture_app_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("PipeWireAudioCaptureApplication");
}

void pipewire_audio_capture_app_load(void)
{
	struct obs_source_info pipewire_audio_capture_application = {
		.id = "pipewire-audio-capture-application",
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
		.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT,
	};

	obs_register_source(&pipewire_audio_capture_application);
}
