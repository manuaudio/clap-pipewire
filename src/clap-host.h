/* clap-pipewire: a CLAP host core for PipeWire's filter-graph */
/* SPDX-FileCopyrightText: Copyright © 2026 manuaudio */
/* SPDX-License-Identifier: MIT */

#ifndef CLAP_PIPEWIRE_HOST_H
#define CLAP_PIPEWIRE_HOST_H

/*
 * The host core knows CLAP and nothing about PipeWire. It loads a bundle,
 * instantiates one plugin, describes it as a flat list of ports (audio
 * channels, input parameters, output parameters, latency) and runs it.
 *
 * The SPA glue (plugin_clap.c) maps those ports onto filter-graph ports. A
 * future out-of-process worker would link this same core and drive it over
 * shared memory, which is why the two are kept apart.
 *
 * Threads, in CLAP's terms:
 *   [main]  the thread that created the instance (PipeWire's main loop)
 *   [audio] the thread that calls cph_instance_process()
 * Functions are tagged below. [audio] functions never allocate, lock or
 * block, and never call into the log directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <clap/clap.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPH_NAME_MAX	128

enum cph_log_level {
	CPH_LOG_ERROR,
	CPH_LOG_WARN,
	CPH_LOG_INFO,
	CPH_LOG_DEBUG,
};

struct cph_log {
	void (*log)(void *data, enum cph_log_level level, const char *msg);
	void *data;
};

/* ---- finding and loading bundles ---------------------------------------- */

/* The CLAP search path: $CLAP_PATH, then ~/.clap, /usr/lib/clap,
 * /usr/local/lib/clap (the CLAP spec's Linux list). Colon separated. */
int cph_search_path(char *buf, size_t size);

/* Resolve a plugin reference to a bundle file. An absolute path is used as
 * is. A relative one ("lsp-plugins.clap", "Cardinal.clap/CardinalFX.clap")
 * is looked up in the search path. Returns 0 or -errno. */
int cph_resolve(const char *name, char *path, size_t size);

struct cph_bundle;

/* [main] dlopen a bundle and init its entry, once per path (ref-counted). */
struct cph_bundle *cph_bundle_open(const char *path, const struct cph_log *log);
void cph_bundle_unref(struct cph_bundle *b);
const char *cph_bundle_path(const struct cph_bundle *b);
uint32_t cph_bundle_n_plugins(const struct cph_bundle *b);
const clap_plugin_descriptor_t *cph_bundle_descriptor(const struct cph_bundle *b, uint32_t index);
/* Find by exact id, then by case-insensitive name. An empty or NULL label
 * picks the only plugin of a single-plugin bundle. */
const clap_plugin_descriptor_t *cph_bundle_find(const struct cph_bundle *b, const char *label);

/* ---- the port layout ---------------------------------------------------- */

enum cph_port_kind {
	CPH_PORT_AUDIO_IN,
	CPH_PORT_AUDIO_OUT,
	CPH_PORT_PARAM_IN,	/* a writable CLAP parameter */
	CPH_PORT_PARAM_OUT,	/* a read-only CLAP parameter (meters, ...) */
	CPH_PORT_LATENCY,	/* the plugin's latency in samples, an output */
};

#define CPH_HINT_BOOLEAN	(1u << 0)
#define CPH_HINT_INTEGER	(1u << 1)

struct cph_port {
	enum cph_port_kind kind;
	char name[CPH_NAME_MAX];	/* unique, safe in "node:port" strings */
	uint32_t slot;			/* index within its kind */

	/* audio */
	uint32_t clap_port;		/* CLAP audio port index */
	uint32_t channel;		/* channel within that port */

	/* parameters */
	clap_id param_id;
	void *cookie;
	uint32_t clap_flags;
	uint32_t hints;
	double min, max, def;
	char label[CPH_NAME_MAX];	/* the CLAP name, unsanitized */
	char module[CPH_NAME_MAX];
};

struct cph_layout {
	uint32_t n_ports;
	struct cph_port *ports;		/* audio in, audio out, params in, params out, latency */

	uint32_t n_audio_in;		/* channels */
	uint32_t n_audio_out;
	uint32_t n_params_in;
	uint32_t n_params_out;

	uint32_t n_clap_in;		/* CLAP audio ports and their widths */
	uint32_t n_clap_out;
	uint32_t *clap_in_channels;
	uint32_t *clap_out_channels;
};

void cph_layout_clear(struct cph_layout *l);
int cph_layout_copy(struct cph_layout *dst, const struct cph_layout *src);
/* 0 when two layouts have the same ports in the same order */
int cph_layout_compare(const struct cph_layout *a, const struct cph_layout *b);
/* Make a filter-graph friendly port name: [A-Za-z0-9_.-], no repeats. */
void cph_sanitize_name(const char *in, char *out, size_t size);

/* ---- instances ---------------------------------------------------------- */

struct cph_instance;

struct cph_instance_opts {
	double sample_rate;
	uint32_t max_frames;		/* CLAP max_frames_count; longer blocks are split */
	const struct cph_log *log;

	/* Ask the owner to call cph_instance_main_callback() on the main thread
	 * soon. Called from any thread, including [audio]: it must be RT-safe.
	 * Return < 0 when it could not be queued. */
	int (*request_main)(void *data, struct cph_instance *inst);
	/* Run fn(arg) on the audio thread and wait for it. NULL means "no audio
	 * thread is running, call it here". */
	void (*run_on_audio)(void *data, void (*fn)(void *arg), void *arg);
	void *data;
};

/* [main] Create, init and describe a plugin. If expect is given, the
 * plugin's layout must match it (the filter-graph descriptor was made from a
 * probe instance and the ports must not move). */
struct cph_instance *cph_instance_new(struct cph_bundle *b, const char *plugin_id,
		const struct cph_instance_opts *opts, const struct cph_layout *expect);
/* [main] Destroys the plugin. Memory is released once no main-thread
 * callback is still queued for this instance. */
void cph_instance_free(struct cph_instance *inst);

const struct cph_layout *cph_instance_layout(const struct cph_instance *inst);
const clap_plugin_descriptor_t *cph_instance_descriptor(const struct cph_instance *inst);
/* The CLAP plugin itself, for extensions the core does not wrap. [main] */
const clap_plugin_t *cph_instance_plugin(const struct cph_instance *inst);

/* [main] or [audio] while not processing. Port index into the layout.
 * Audio: float[n]; params in: const float*; params out and latency: float*. */
void cph_instance_connect(struct cph_instance *inst, uint32_t port, void *data);

/* [main] */
int cph_instance_activate(struct cph_instance *inst);
void cph_instance_deactivate(struct cph_instance *inst);
uint32_t cph_instance_latency(const struct cph_instance *inst);

/* [audio] Process n frames. Input parameter ports are compared with what
 * was last sent and changes become CLAP_EVENT_PARAM_VALUE events at frame 0.
 * Never fails: while the plugin is not running, audio passes through dry. */
void cph_instance_process(struct cph_instance *inst, uint32_t n_frames);

/* [main] Service on_main_thread, restarts and the audio-thread log. */
void cph_instance_main_callback(struct cph_instance *inst);

/* [main] State as an opaque file (the plugin's own clap.state format). */
int cph_instance_state_save(struct cph_instance *inst, const char *path);
int cph_instance_state_load(struct cph_instance *inst, const char *path);
bool cph_instance_state_dirty(const struct cph_instance *inst);

/* [main] The plugin's current value of a parameter port (params.get_value). */
int cph_instance_param_value(struct cph_instance *inst, uint32_t port, double *value);
/* [main] value_to_text for a parameter port. */
int cph_instance_param_text(struct cph_instance *inst, uint32_t port, double value,
		char *buf, size_t size);
/* [main] text_to_value for a parameter port ("500 Hz", "Lo-pass", ...). */
int cph_instance_param_parse(struct cph_instance *inst, uint32_t port, const char *text,
		double *value);
/* [main, inactive] Set a parameter now, through params.flush. */
int cph_instance_param_set(struct cph_instance *inst, uint32_t port, double value);

/* Port index by filter-graph name, or by exact CLAP name. -1 if none. */
int cph_layout_find(const struct cph_layout *l, const char *name);

struct cph_stats {
	uint32_t process_errors;	/* CLAP_PROCESS_ERROR or start_processing failures */
	uint32_t log_dropped;		/* audio-thread log lines lost to a full ring */
	uint32_t restarts;
	uint64_t frames;
};
void cph_instance_stats(const struct cph_instance *inst, struct cph_stats *stats);

/* [main] Probe a plugin's layout with a short-lived instance. */
int cph_probe_layout(struct cph_bundle *b, const char *plugin_id, double sample_rate,
		const struct cph_log *log, struct cph_layout *out);

#ifdef __cplusplus
}
#endif

#endif
