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
//Based on the linux-pulseaudio plugin design by Leonhard Oelke

#include <pipewire/pipewire.h>

/**
 * Initialize a PipeWire threadloop and context if there isn't one
 * and increase the reference count
 */
void pipewire_init();

/**
 * Decrease the PipeWire reference count
 * If it reaches 0 destroy the PipeWire context and threadloop
 */
void pipewire_unref();

/**
 * Lock the PipeWire threadloop
 */
void pipewire_lock();

/**
 * Unlock the PipeWire threadloop
 */
void pipewire_unlock();

/**
 * Wait for callbacks to complete
 */
void pipewire_wait();

/**
 * Called from a callback to let pipewire_wait know that it completed
 */
void pipewire_continue();

/**
 * Add a listener to a new PipeWire registry
 * @param call_now Call the callbacks now
 * @returns The new registry proxy, needs to be destroyed when no longer needed
 */
struct pw_registry *
pipewire_add_registry_listener(bool call_now, struct spa_hook *hook,
			       const struct pw_registry_events *callbacks,
			       void *data);

/**
 * Destroy a PipeWire proxy
 **/
void pipewire_proxy_destroy(struct pw_proxy *proxy);

/**
 * Crate and add a listener to a PipeWire stream
 * that can be connected to a node later in order to get data from it
 */
struct pw_stream *pipewire_stream_new(struct pw_properties *props,
				      struct spa_hook *stream_listener,
				      const struct pw_stream_events *callbacks,
				      void *data);

/**
 * Connect a stream to a PipeWire node
 * @returns 0 on success
 */
int pipewire_stream_connect(struct pw_stream *stream,
			    enum spa_direction direction, uint32_t target_id,
			    enum pw_stream_flags flags,
			    const struct spa_pod **params, uint32_t n_params);
/**
 * Disconnect a stream from a PipeWire node
 * @returns 0 on success
 */
int pipewire_stream_disconnect(struct pw_stream *stream);
/**
 * Disconnect and destroy a stream
 */
void pipewire_stream_destroy(struct pw_stream *stream);