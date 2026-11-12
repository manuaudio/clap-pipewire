/* clap-pipewire: CLAP plugins in PipeWire's filter-graph */
/* SPDX-FileCopyrightText: Copyright © 2026 manuaudio */
/* SPDX-License-Identifier: MIT */

/*
 * libspa-filter-graph-plugin-clap.so
 *
 * filter-graph resolves `type = clap` to
 * "filter-graph/libspa-filter-graph-plugin-clap" in SPA_PLUGIN_DIR (a colon
 * separated list) and asks it for factory "filter.graph.plugin.clap", the
 * same way it loads its own lv2 and ladspa plugins.
 *
 *   {
 *     type   = clap
 *     name   = fx
 *     plugin = "lsp-plugins.clap"          # or an absolute path
 *     label  = "in.lsp-plug.filter_stereo" # CLAP plugin id (or name)
 *     control = { "Frequency" = 500 }      # CLAP params, sanitized names
 *     config = {
 *       params     = { "Frequency" = "500 Hz" }  # via the plugin's text_to_value
 *       state      = "/path/fx.state"      # load on start (clap.state)
 *       save-state = true                  # write it back when torn down
 *       max-block  = 8192                  # CLAP max_frames_count
 *     }
 *   }
 *
 * Threads: filter-graph calls make_desc, instantiate, activate, deactivate,
 * cleanup and control changes from PipeWire's main loop, and run from the
 * data loop, with the graph switched off around (de)activation. That maps
 * directly on CLAP's [main-thread] and [audio-thread].
 */

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/utils/defs.h>
#include <spa/utils/result.h>
#include <spa/utils/json.h>
#include <spa/utils/string.h>

#include "fga/audio-plugin.h"
#include "clap-host.h"

#define PROBE_RATE	48000.0

#ifndef FGA_ABI_SERIES
#define FGA_ABI_SERIES	"1.6."
#endif

struct plugin {
	struct spa_handle handle;
	struct spa_fga_plugin plugin;

	struct spa_log *log;
	struct spa_loop *data_loop;
	struct spa_loop *main_loop;

	struct cph_log clog;
	struct cph_bundle *bundle;
	char path[PATH_MAX];
};

struct descriptor {
	struct spa_fga_descriptor desc;
	struct plugin *p;
	struct cph_layout layout;
};

struct instance {
	struct descriptor *d;
	struct plugin *p;
	struct cph_instance *ci;
	char *state_path;
	bool save_state;
};

static void log_cb(void *data, enum cph_log_level level, const char *msg)
{
	struct plugin *p = data;
	enum spa_log_level l;

	switch (level) {
	case CPH_LOG_ERROR: l = SPA_LOG_LEVEL_ERROR; break;
	case CPH_LOG_WARN: l = SPA_LOG_LEVEL_WARN; break;
	case CPH_LOG_INFO: l = SPA_LOG_LEVEL_INFO; break;
	default: l = SPA_LOG_LEVEL_DEBUG; break;
	}
	spa_log_lev(p->log, l, "clap: %s", msg);
}

/* ---- thread plumbing ---------------------------------------------------- */

static int do_main_callback(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	cph_instance_main_callback(user_data);
	return 0;
}

/* Called from any thread, including the data thread: spa_loop_invoke with
 * block=false is PipeWire's own lock-free way to hand work to a loop. */
static int request_main(void *data, struct cph_instance *ci)
{
	struct plugin *p = data;
	if (p->main_loop == NULL)
		return -ENOTSUP;
	return spa_loop_invoke(p->main_loop, do_main_callback, 0, NULL, 0, false, ci);
}

struct audio_call {
	void (*fn)(void *arg);
	void *arg;
};

static int do_audio_call(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	const struct audio_call *c = data;
	c->fn(c->arg);
	return 0;
}

static void run_on_audio(void *data, void (*fn)(void *arg), void *arg)
{
	struct plugin *p = data;
	struct audio_call c = { fn, arg };
	if (p->data_loop == NULL ||
	    spa_loop_invoke(p->data_loop, do_audio_call, 0, &c, sizeof(c), true, NULL) < 0)
		fn(arg);
}

/* ---- config ------------------------------------------------------------- */

static int parse_config(struct instance *i, const char *config, uint32_t *max_block)
{
	struct spa_json it[1];
	const char *val;
	char key[256];
	int len;

	if (config == NULL)
		return 0;
	if (spa_json_begin_object(&it[0], config, strlen(config)) <= 0) {
		spa_log_error(i->p->log, "clap: config must be an object");
		return -EINVAL;
	}
	while ((len = spa_json_object_next(&it[0], key, sizeof(key), &val)) > 0) {
		if (spa_streq(key, "state")) {
			char path[PATH_MAX];
			if (spa_json_parse_stringn(val, len, path, sizeof(path)) <= 0)
				return -EINVAL;
			free(i->state_path);
			i->state_path = strdup(path);
		} else if (spa_streq(key, "save-state")) {
			bool b;
			if (spa_json_parse_bool(val, len, &b) <= 0)
				return -EINVAL;
			i->save_state = b;
		} else if (spa_streq(key, "params")) {
			if (!spa_json_is_object(val, len))
				return -EINVAL;
			/* applied after the instance exists, see apply_params() */
		} else if (spa_streq(key, "max-block")) {
			int v;
			if (spa_json_parse_int(val, len, &v) <= 0 || v < 16 || v > 65536)
				return -EINVAL;
			*max_block = (uint32_t)v;
		} else {
			spa_log_warn(i->p->log, "clap: unknown config key '%s'", key);
		}
	}
	return 0;
}

/* config.params = { "Frequency" = "500 Hz"  "Filter_type" = 0 }
 * Strings go through the plugin's own text_to_value, numbers are plain
 * values. Set through params.flush before activation, after any state. */
static void apply_params(struct instance *i, const char *config)
{
	struct spa_json it[2];
	const char *val;
	char key[256];
	int len;

	if (config == NULL || spa_json_begin_object(&it[0], config, strlen(config)) <= 0)
		return;
	if ((len = spa_json_object_find(&it[0], "params", &val)) <= 0 ||
	    !spa_json_is_object(val, len) ||
	    spa_json_begin_object(&it[1], val, strlen(val)) <= 0)
		return;
	while ((len = spa_json_object_next(&it[1], key, sizeof(key), &val)) > 0) {
		const struct cph_layout *l = cph_instance_layout(i->ci);
		int port = cph_layout_find(l, key), res;
		char text[256];
		double v;
		float f;

		if (port < 0) {
			spa_log_warn(i->p->log, "clap: %s: no parameter '%s'", i->d->desc.name, key);
			continue;
		}
		if (spa_json_is_string(val, len)) {
			if (spa_json_parse_stringn(val, len, text, sizeof(text)) <= 0 ||
			    cph_instance_param_parse(i->ci, (uint32_t)port, text, &v) < 0) {
				spa_log_warn(i->p->log, "clap: %s: can't parse '%.*s' for %s",
						i->d->desc.name, len, val, key);
				continue;
			}
		} else if (spa_json_parse_float(val, len, &f) > 0) {
			v = f;
		} else {
			spa_log_warn(i->p->log, "clap: %s: bad value for %s", i->d->desc.name, key);
			continue;
		}
		if ((res = cph_instance_param_set(i->ci, (uint32_t)port, v)) < 0)
			spa_log_warn(i->p->log, "clap: %s: can't set %s: %s", i->d->desc.name,
					key, spa_strerror(res));
		else
			spa_log_info(i->p->log, "clap: %s: %s = %f", i->d->desc.name, key, v);
	}
}

/* ---- descriptor callbacks ----------------------------------------------- */

static void *clap_instantiate(const struct spa_fga_plugin *plugin,
		const struct spa_fga_descriptor *desc, unsigned long SampleRate,
		int index, const char *config)
{
	struct descriptor *d = (struct descriptor *)desc;
	struct plugin *p = d->p;
	struct instance *i;
	uint32_t max_block = 0;
	int res;

	i = calloc(1, sizeof(*i));
	if (i == NULL)
		return NULL;
	i->d = d;
	i->p = p;

	if ((res = parse_config(i, config, &max_block)) < 0)
		goto error;

	struct cph_instance_opts opts = {
		.sample_rate = (double)SampleRate,
		.max_frames = max_block,
		.log = &p->clog,
		.request_main = request_main,
		.run_on_audio = run_on_audio,
		.data = p,
	};
	i->ci = cph_instance_new(p->bundle, desc->name, &opts, &d->layout);
	if (i->ci == NULL) {
		res = errno ? -errno : -EIO;
		goto error;
	}
	if (i->state_path) {
		res = cph_instance_state_load(i->ci, i->state_path);
		if (res == -ENOENT)
			spa_log_info(p->log, "clap: %s: no state yet at %s", desc->name, i->state_path);
		else if (res < 0)
			spa_log_warn(p->log, "clap: %s: can't load state %s: %s",
					desc->name, i->state_path, spa_strerror(res));
		else
			spa_log_info(p->log, "clap: %s: state loaded from %s", desc->name, i->state_path);
	}
	apply_params(i, config);
	spa_log_info(p->log, "clap: instantiated %s[%d] at %lu Hz", desc->name, index, SampleRate);
	return i;

error:
	spa_log_error(p->log, "clap: can't instantiate %s: %s", desc->name, spa_strerror(res));
	free(i->state_path);
	free(i);
	errno = -res;
	return NULL;
}

static void clap_cleanup(void *instance)
{
	struct instance *i = instance;
	struct cph_stats st;

	cph_instance_deactivate(i->ci);
	if (i->state_path && i->save_state) {
		int res = cph_instance_state_save(i->ci, i->state_path);
		if (res < 0)
			spa_log_warn(i->p->log, "clap: can't save state to %s: %s",
					i->state_path, spa_strerror(res));
		else
			spa_log_info(i->p->log, "clap: state saved to %s", i->state_path);
	}
	cph_instance_stats(i->ci, &st);
	if (st.process_errors || st.log_dropped)
		spa_log_warn(i->p->log, "clap: %s: %u process errors, %u audio-thread log lines dropped",
				i->d->desc.name, st.process_errors, st.log_dropped);
	cph_instance_free(i->ci);
	free(i->state_path);
	free(i);
}

static void clap_connect_port(void *instance, unsigned long port, void *data)
{
	struct instance *i = instance;
	cph_instance_connect(i->ci, (uint32_t)port, data);
}

static void clap_activate(void *instance)
{
	struct instance *i = instance;
	int res;
	if ((res = cph_instance_activate(i->ci)) < 0)
		spa_log_error(i->p->log, "clap: %s: activate failed (%s); passing audio through",
				i->d->desc.name, spa_strerror(res));
}

static void clap_deactivate(void *instance)
{
	struct instance *i = instance;
	cph_instance_deactivate(i->ci);
}

static void clap_run(void *instance, unsigned long SampleCount)
{
	struct instance *i = instance;
	cph_instance_process(i->ci, (uint32_t)SampleCount);
}

static void clap_free(const struct spa_fga_descriptor *desc)
{
	struct descriptor *d = (struct descriptor *)desc;
	free((char *)d->desc.name);
	free(d->desc.ports);
	cph_layout_clear(&d->layout);
	free(d);
}

static const struct spa_fga_descriptor *clap_make_desc(void *plugin, const char *name)
{
	struct plugin *p = plugin;
	const clap_plugin_descriptor_t *cd;
	struct descriptor *d;
	uint32_t i;
	int res;

	if ((cd = cph_bundle_find(p->bundle, name)) == NULL) {
		uint32_t n = cph_bundle_n_plugins(p->bundle);
		spa_log_error(p->log, "clap: %s has no plugin '%s'%s", p->path, name,
				(name == NULL || *name == '\0') && n > 1 ?
				"; this bundle has several, set label to a plugin id" : "");
		for (i = 0; i < n && i < 50; i++) {
			const clap_plugin_descriptor_t *x = cph_bundle_descriptor(p->bundle, i);
			spa_log_info(p->log, "clap:   %s  (%s)", x->id, x->name);
		}
		errno = ENOENT;
		return NULL;
	}

	d = calloc(1, sizeof(*d));
	if (d == NULL)
		return NULL;
	d->p = p;
	if ((res = cph_probe_layout(p->bundle, cd->id, PROBE_RATE, &p->clog, &d->layout)) < 0) {
		spa_log_error(p->log, "clap: can't probe %s: %s", cd->id, spa_strerror(res));
		free(d);
		errno = -res;
		return NULL;
	}

	d->desc.name = strdup(cd->id);
	d->desc.flags = 0;
	d->desc.instantiate = clap_instantiate;
	d->desc.cleanup = clap_cleanup;
	d->desc.connect_port = clap_connect_port;
	d->desc.activate = clap_activate;
	d->desc.deactivate = clap_deactivate;
	d->desc.run = clap_run;
	d->desc.free = clap_free;
	d->desc.n_ports = d->layout.n_ports;
	d->desc.ports = calloc(d->desc.n_ports, sizeof(struct spa_fga_port));
	if (d->desc.name == NULL || d->desc.ports == NULL) {
		clap_free(&d->desc);
		errno = ENOMEM;
		return NULL;
	}

	for (i = 0; i < d->layout.n_ports; i++) {
		const struct cph_port *cp = &d->layout.ports[i];
		struct spa_fga_port *fp = &d->desc.ports[i];

		fp->index = i;
		fp->name = cp->name;	/* owned by d->layout */
		switch (cp->kind) {
		case CPH_PORT_AUDIO_IN:
			fp->flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO;
			break;
		case CPH_PORT_AUDIO_OUT:
			fp->flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO;
			break;
		case CPH_PORT_PARAM_IN:
			fp->flags = SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL;
			break;
		case CPH_PORT_PARAM_OUT:
			fp->flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL;
			break;
		case CPH_PORT_LATENCY:
			fp->flags = SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL;
			fp->hint |= SPA_FGA_HINT_LATENCY;
			break;
		}
		if (cp->hints & CPH_HINT_BOOLEAN)
			fp->hint |= SPA_FGA_HINT_BOOLEAN;
		if (cp->hints & CPH_HINT_INTEGER)
			fp->hint |= SPA_FGA_HINT_INTEGER;
		fp->min = (float)cp->min;
		fp->max = (float)cp->max;
		fp->def = (float)cp->def;
	}
	spa_log_info(p->log, "clap: %s \"%s\": %u in, %u out, %u params, %u read-only",
			cd->id, cd->name, d->layout.n_audio_in, d->layout.n_audio_out,
			d->layout.n_params_in, d->layout.n_params_out);
	return &d->desc;
}

static const struct spa_fga_plugin_methods impl_plugin = {
	SPA_VERSION_FGA_PLUGIN_METHODS,
	.make_desc = clap_make_desc,
};

/* ---- SPA handle --------------------------------------------------------- */

/* audio-plugin.h is not a stable ABI and its version numbers do not move
 * when it changes (PipeWire 1.7 changed instantiate() without bumping
 * SPA_VERSION_FGA_PLUGIN_METHODS). Refuse to load into another release
 * series rather than crash it. CLAP_PW_IGNORE_ABI=1 overrides. */
static int check_abi(struct plugin *p)
{
	const char *(*get_version)(void);
	const char *v, *ignore = getenv("CLAP_PW_IGNORE_ABI");

	get_version = (const char *(*)(void))dlsym(RTLD_DEFAULT, "pw_get_library_version");
	if (get_version == NULL || (v = get_version()) == NULL)
		return 0;	/* not inside a PipeWire process (tests) */
	if (strncmp(v, FGA_ABI_SERIES, strlen(FGA_ABI_SERIES)) == 0)
		return 0;
	if (ignore && spa_atob(ignore)) {
		spa_log_warn(p->log, "clap: built for PipeWire %sx, running in %s: "
				"CLAP_PW_IGNORE_ABI set, loading anyway", FGA_ABI_SERIES, v);
		return 0;
	}
	spa_log_error(p->log, "clap: built for the PipeWire %sx filter-graph plugin ABI, "
			"but this is PipeWire %s; rebuild clap-pipewire for it", FGA_ABI_SERIES, v);
	return -ENOTSUP;
}

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct plugin *p = (struct plugin *)handle;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	if (spa_streq(type, SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin))
		*interface = &p->plugin;
	else
		return -ENOENT;
	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct plugin *p = (struct plugin *)handle;
	cph_bundle_unref(p->bundle);
	p->bundle = NULL;
	return 0;
}

static size_t impl_get_size(const struct spa_handle_factory *factory, const struct spa_dict *params)
{
	return sizeof(struct plugin);
}

static int impl_init(const struct spa_handle_factory *factory, struct spa_handle *handle,
		const struct spa_dict *info, const struct spa_support *support, uint32_t n_support)
{
	struct plugin *p = (struct plugin *)handle;
	const char *ref = NULL;
	uint32_t i;
	int res;

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	p->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	p->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	p->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);
	p->clog.log = log_cb;
	p->clog.data = p;

	if ((res = check_abi(p)) < 0)
		return res;

	for (i = 0; info && i < info->n_items; i++) {
		if (spa_streq(info->items[i].key, "filter.graph.path"))
			ref = info->items[i].value;
	}
	if (ref == NULL || *ref == '\0') {
		spa_log_error(p->log, "clap: missing plugin (a .clap bundle)");
		return -EINVAL;
	}
	if ((res = cph_resolve(ref, p->path, sizeof(p->path))) < 0) {
		char sp[4096];
		cph_search_path(sp, sizeof(sp));
		spa_log_error(p->log, "clap: can't find bundle '%s' in %s: %s",
				ref, ref[0] == '/' ? "the filesystem" : sp, spa_strerror(res));
		return res;
	}
	if ((p->bundle = cph_bundle_open(p->path, &p->clog)) == NULL)
		return -EIO;

	p->plugin.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin,
			SPA_VERSION_FGA_PLUGIN, &impl_plugin, p);
	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{ SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin },
};

static int impl_enum_interface_info(const struct spa_handle_factory *factory,
		const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;
	*info = &impl_interfaces[(*index)++];
	return 1;
}

static const struct spa_handle_factory clap_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	"filter.graph.plugin.clap",
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index > 0)
		return 0;
	*factory = &clap_factory;
	(*index)++;
	return 1;
}
