/* clap-pipewire: a CLAP host core for PipeWire's filter-graph */
/* SPDX-FileCopyrightText: Copyright © 2026 manuaudio */
/* SPDX-License-Identifier: MIT */

#include "clap-host.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CPH_VERSION
#define CPH_VERSION "0.0.0"
#endif

#define DEFAULT_RATE		48000.0
#define DEFAULT_MAX_FRAMES	8192u
#define MAX_CHANNELS		64u
#define LOG_RING		32u
#define LOG_LINE		256u

/* ---- logging ------------------------------------------------------------ */

static void cph_logv(const struct cph_log *log, enum cph_log_level level,
		const char *fmt, va_list ap)
{
	char buf[1024];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	if (log && log->log)
		log->log(log->data, level, buf);
	else if (level <= CPH_LOG_WARN)
		fprintf(stderr, "clap-pipewire: %s\n", buf);
}

__attribute__((format(printf, 3, 4)))
static void cph_logf(const struct cph_log *log, enum cph_log_level level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	cph_logv(log, level, fmt, ap);
	va_end(ap);
}

/* ---- search path -------------------------------------------------------- */

static void append(char *buf, size_t size, size_t *pos, const char *s)
{
	int n;
	if (s == NULL || *s == '\0' || *pos >= size)
		return;
	n = snprintf(buf + *pos, size - *pos, "%s%s", *pos ? ":" : "", s);
	if (n > 0)
		*pos += (size_t)n;
}

int cph_search_path(char *buf, size_t size)
{
	char home[PATH_MAX];
	const char *h = getenv("HOME");
	size_t pos = 0;

	if (size == 0)
		return -EINVAL;
	buf[0] = '\0';
	append(buf, size, &pos, getenv("CLAP_PATH"));
	if (h && *h) {
		snprintf(home, sizeof(home), "%s/.clap", h);
		append(buf, size, &pos, home);
	}
	append(buf, size, &pos, "/usr/lib/clap");
	append(buf, size, &pos, "/usr/local/lib/clap");
	return pos >= size ? -ENAMETOOLONG : 0;
}

/* A file, or a directory bundle "X.clap/" that holds "X.clap/X.clap". */
static int try_candidate(const char *cand, char *path, size_t size)
{
	struct stat st;
	char inner[PATH_MAX];
	const char *base;

	if (stat(cand, &st) < 0)
		return -errno;
	if (S_ISREG(st.st_mode)) {
		if ((size_t)snprintf(path, size, "%s", cand) >= size)
			return -ENAMETOOLONG;
		return 0;
	}
	if (!S_ISDIR(st.st_mode))
		return -ENOENT;
	base = strrchr(cand, '/');
	base = base ? base + 1 : cand;
	if ((size_t)snprintf(inner, sizeof(inner), "%s/%s", cand, base) >= sizeof(inner))
		return -ENAMETOOLONG;
	if (stat(inner, &st) < 0 || !S_ISREG(st.st_mode))
		return -EISDIR;
	if ((size_t)snprintf(path, size, "%s", inner) >= size)
		return -ENAMETOOLONG;
	return 0;
}

int cph_resolve(const char *name, char *path, size_t size)
{
	char sp[4096], cand[PATH_MAX];
	const char *s, *e;
	int res = -ENOENT;

	if (name == NULL || *name == '\0')
		return -EINVAL;
	if (name[0] == '/')
		return try_candidate(name, path, size);
	if (strstr(name, "..") != NULL)
		return -EINVAL;

	if ((res = cph_search_path(sp, sizeof(sp))) < 0)
		return res;
	res = -ENOENT;
	for (s = sp; *s; s = *e ? e + 1 : e) {
		int r;
		e = strchr(s, ':');
		if (e == NULL)
			e = s + strlen(s);
		if (e == s)
			continue;
		if ((size_t)snprintf(cand, sizeof(cand), "%.*s/%s", (int)(e - s), s, name) >= sizeof(cand))
			continue;
		if ((r = try_candidate(cand, path, size)) == 0)
			return 0;
		if (r == -EISDIR)
			res = r;
	}
	return res;
}

/* ---- bundles ------------------------------------------------------------ */

struct cph_bundle {
	struct cph_bundle *next;
	int ref;
	char *path;
	void *dl;
	const clap_plugin_entry_t *entry;
	const clap_plugin_factory_t *factory;
};

static pthread_mutex_t bundles_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cph_bundle *bundles;

struct cph_bundle *cph_bundle_open(const char *path, const struct cph_log *log)
{
	struct cph_bundle *b;
	char real[PATH_MAX];

	if (realpath(path, real) == NULL) {
		cph_logf(log, CPH_LOG_ERROR, "%s: %s", path, strerror(errno));
		return NULL;
	}

	pthread_mutex_lock(&bundles_lock);
	for (b = bundles; b; b = b->next) {
		if (strcmp(b->path, real) == 0) {
			b->ref++;
			pthread_mutex_unlock(&bundles_lock);
			return b;
		}
	}

	b = calloc(1, sizeof(*b));
	if (b == NULL)
		goto error;
	b->path = strdup(real);
	/* RTLD_NODELETE: plugins leave threads and atexit handlers behind more
	 * often than not; keeping the code mapped makes unloading harmless. */
	b->dl = dlopen(real, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
	if (b->dl == NULL) {
		cph_logf(log, CPH_LOG_ERROR, "dlopen %s: %s", real, dlerror());
		goto error;
	}
	b->entry = dlsym(b->dl, "clap_entry");
	if (b->entry == NULL) {
		cph_logf(log, CPH_LOG_ERROR, "%s: no clap_entry symbol", real);
		goto error;
	}
	if (!clap_version_is_compatible(b->entry->clap_version)) {
		cph_logf(log, CPH_LOG_ERROR, "%s: incompatible CLAP version %u.%u.%u", real,
				b->entry->clap_version.major, b->entry->clap_version.minor,
				b->entry->clap_version.revision);
		goto error;
	}
	if (!b->entry->init(real)) {
		cph_logf(log, CPH_LOG_ERROR, "%s: clap_entry.init failed", real);
		b->entry = NULL;
		goto error;
	}
	b->factory = b->entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
	if (b->factory == NULL) {
		cph_logf(log, CPH_LOG_ERROR, "%s: no plugin factory", real);
		goto error;
	}
	b->ref = 1;
	b->next = bundles;
	bundles = b;
	pthread_mutex_unlock(&bundles_lock);
	cph_logf(log, CPH_LOG_INFO, "opened %s: %u plugin(s), CLAP %u.%u.%u", real,
			b->factory->get_plugin_count(b->factory),
			b->entry->clap_version.major, b->entry->clap_version.minor,
			b->entry->clap_version.revision);
	return b;

error:
	if (b) {
		if (b->entry)
			b->entry->deinit();
		if (b->dl)
			dlclose(b->dl);
		free(b->path);
		free(b);
	}
	pthread_mutex_unlock(&bundles_lock);
	return NULL;
}

void cph_bundle_unref(struct cph_bundle *b)
{
	struct cph_bundle **pp;

	if (b == NULL)
		return;
	pthread_mutex_lock(&bundles_lock);
	if (--b->ref > 0) {
		pthread_mutex_unlock(&bundles_lock);
		return;
	}
	for (pp = &bundles; *pp; pp = &(*pp)->next) {
		if (*pp == b) {
			*pp = b->next;
			break;
		}
	}
	pthread_mutex_unlock(&bundles_lock);

	b->entry->deinit();
	dlclose(b->dl);
	free(b->path);
	free(b);
}

static void bundle_ref(struct cph_bundle *b)
{
	pthread_mutex_lock(&bundles_lock);
	b->ref++;
	pthread_mutex_unlock(&bundles_lock);
}

const char *cph_bundle_path(const struct cph_bundle *b)
{
	return b->path;
}

uint32_t cph_bundle_n_plugins(const struct cph_bundle *b)
{
	return b->factory->get_plugin_count(b->factory);
}

const clap_plugin_descriptor_t *cph_bundle_descriptor(const struct cph_bundle *b, uint32_t index)
{
	return b->factory->get_plugin_descriptor(b->factory, index);
}

const clap_plugin_descriptor_t *cph_bundle_find(const struct cph_bundle *b, const char *label)
{
	uint32_t i, n = cph_bundle_n_plugins(b);
	const clap_plugin_descriptor_t *d;

	if (label == NULL || *label == '\0')
		return n == 1 ? cph_bundle_descriptor(b, 0) : NULL;
	for (i = 0; i < n; i++) {
		d = cph_bundle_descriptor(b, i);
		if (d && d->id && strcmp(d->id, label) == 0)
			return d;
	}
	for (i = 0; i < n; i++) {
		d = cph_bundle_descriptor(b, i);
		if (d && d->name && strcasecmp(d->name, label) == 0)
			return d;
	}
	return NULL;
}

/* ---- layouts ------------------------------------------------------------ */

void cph_sanitize_name(const char *in, char *out, size_t size)
{
	size_t o = 0;
	bool under = false;

	if (size == 0)
		return;
	for (; in && *in && o + 1 < size; in++) {
		char c = *in;
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '.';
		if (ok) {
			out[o++] = c;
			under = false;
		} else if (!under && o > 0) {
			out[o++] = '_';
			under = true;
		}
	}
	while (o > 0 && out[o - 1] == '_')
		o--;
	out[o] = '\0';
}

void cph_layout_clear(struct cph_layout *l)
{
	free(l->ports);
	free(l->clap_in_channels);
	free(l->clap_out_channels);
	memset(l, 0, sizeof(*l));
}

int cph_layout_copy(struct cph_layout *dst, const struct cph_layout *src)
{
	*dst = *src;
	dst->ports = calloc(src->n_ports ? src->n_ports : 1, sizeof(struct cph_port));
	dst->clap_in_channels = calloc(src->n_clap_in ? src->n_clap_in : 1, sizeof(uint32_t));
	dst->clap_out_channels = calloc(src->n_clap_out ? src->n_clap_out : 1, sizeof(uint32_t));
	if (!dst->ports || !dst->clap_in_channels || !dst->clap_out_channels) {
		cph_layout_clear(dst);
		return -ENOMEM;
	}
	memcpy(dst->ports, src->ports, src->n_ports * sizeof(struct cph_port));
	memcpy(dst->clap_in_channels, src->clap_in_channels, src->n_clap_in * sizeof(uint32_t));
	memcpy(dst->clap_out_channels, src->clap_out_channels, src->n_clap_out * sizeof(uint32_t));
	return 0;
}

int cph_layout_compare(const struct cph_layout *a, const struct cph_layout *b)
{
	uint32_t i;

	if (a->n_ports != b->n_ports || a->n_clap_in != b->n_clap_in ||
	    a->n_clap_out != b->n_clap_out)
		return 1;
	for (i = 0; i < a->n_clap_in; i++)
		if (a->clap_in_channels[i] != b->clap_in_channels[i])
			return 1;
	for (i = 0; i < a->n_clap_out; i++)
		if (a->clap_out_channels[i] != b->clap_out_channels[i])
			return 1;
	for (i = 0; i < a->n_ports; i++) {
		const struct cph_port *p = &a->ports[i], *q = &b->ports[i];
		if (p->kind != q->kind || strcmp(p->name, q->name) != 0 ||
		    p->param_id != q->param_id)
			return 1;
	}
	return 0;
}

static bool name_taken(const struct cph_port *ports, uint32_t n, const char *name)
{
	uint32_t i;
	for (i = 0; i < n; i++)
		if (strcmp(ports[i].name, name) == 0)
			return true;
	return false;
}

/* ---- instances ---------------------------------------------------------- */

enum run_state {
	RUN_INACTIVE,
	RUN_ACTIVE,
	RUN_STOP_REQUESTED,	/* main asked the audio thread to stop for a restart */
	RUN_STOPPED,		/* audio thread stopped; main may deactivate/activate */
};

struct param_map {
	clap_id id;
	uint32_t port;
};

struct cph_instance {
	struct cph_bundle *bundle;
	struct cph_instance_opts opts;
	struct cph_log log;
	pthread_t main_thread;

	clap_host_t host;
	const clap_plugin_t *plugin;
	const clap_plugin_params_t *params;
	const clap_plugin_audio_ports_t *audio_ports;
	const clap_plugin_latency_t *latency;
	const clap_plugin_state_t *state;

	struct cph_layout layout;
	uint32_t first_param_in, first_param_out, latency_port;

	/* what the owner connected */
	float **in_ptr;
	float **out_ptr;
	const float **pin_ptr;
	float **pout_ptr;
	float *latency_ptr;

	/* [audio] */
	float *pin_last;
	clap_event_param_value_t *ev;
	uint32_t n_ev, n_ev_cur;
	clap_audio_buffer_t *ain, *aout;
	float **ain_ch, **aout_ch;
	float *scratch;			/* n_audio_in * max_frames, for aliased buffers */
	float *silence, *discard;
	bool processing;
	int64_t steady;

	clap_input_events_t in_events;
	clap_output_events_t out_events;
	clap_input_events_t empty_events;

	struct param_map *map;		/* sorted by id */
	uint32_t n_map;

	/* [main] */
	bool activated;
	bool baseline_done;
	bool zombie;
	bool state_dirty;
	uint32_t latency_frames;

	/* cross-thread */
	atomic_int run;
	atomic_int main_pending;
	atomic_int callback_req;
	atomic_int restart_req;
	atomic_int flush_req;
	atomic_uint process_errors;
	atomic_uint log_dropped;
	atomic_uint restarts;
	atomic_uint_fast64_t frames;

	/* audio-thread log ring, single producer (audio) single consumer (main) */
	char log_line[LOG_RING][LOG_LINE];
	uint8_t log_level[LOG_RING];
	atomic_uint log_head, log_tail;
};

static _Thread_local int tls_audio;

static void schedule_main(struct cph_instance *inst)
{
	if (atomic_exchange(&inst->main_pending, 1) != 0)
		return;
	if (inst->opts.request_main == NULL ||
	    inst->opts.request_main(inst->opts.data, inst) < 0)
		atomic_store(&inst->main_pending, 0);
}

/* host: core */

static const void *host_get_extension(const clap_host_t *host, const char *id);

static void host_request_restart(const clap_host_t *host)
{
	struct cph_instance *inst = host->host_data;
	atomic_store(&inst->restart_req, 1);
	schedule_main(inst);
}

static void host_request_process(const clap_host_t *host)
{
	/* we process every cycle anyway */
	(void)host;
}

static void host_request_callback(const clap_host_t *host)
{
	struct cph_instance *inst = host->host_data;
	atomic_store(&inst->callback_req, 1);
	schedule_main(inst);
}

/* host: log */

static enum cph_log_level map_severity(clap_log_severity s)
{
	switch (s) {
	case CLAP_LOG_DEBUG: return CPH_LOG_DEBUG;
	case CLAP_LOG_INFO: return CPH_LOG_INFO;
	case CLAP_LOG_WARNING: return CPH_LOG_WARN;
	default: return CPH_LOG_ERROR;
	}
}

static void host_log(const clap_host_t *host, clap_log_severity severity, const char *msg)
{
	struct cph_instance *inst = host->host_data;
	enum cph_log_level level = map_severity(severity);

	if (tls_audio) {
		unsigned h = atomic_load_explicit(&inst->log_head, memory_order_relaxed);
		unsigned t = atomic_load_explicit(&inst->log_tail, memory_order_acquire);
		if (h - t >= LOG_RING) {
			atomic_fetch_add_explicit(&inst->log_dropped, 1, memory_order_relaxed);
			return;
		}
		strncpy(inst->log_line[h % LOG_RING], msg ? msg : "", LOG_LINE - 1);
		inst->log_line[h % LOG_RING][LOG_LINE - 1] = '\0';
		inst->log_level[h % LOG_RING] = (uint8_t)level;
		atomic_store_explicit(&inst->log_head, h + 1, memory_order_release);
		schedule_main(inst);
		return;
	}
	cph_logf(&inst->log, level, "[%s] %s", inst->plugin && inst->plugin->desc ?
			inst->plugin->desc->id : "plugin", msg ? msg : "");
}

static void flush_log(struct cph_instance *inst)
{
	unsigned t = atomic_load_explicit(&inst->log_tail, memory_order_relaxed);
	unsigned h = atomic_load_explicit(&inst->log_head, memory_order_acquire);

	for (; t != h; t++)
		cph_logf(&inst->log, (enum cph_log_level)inst->log_level[t % LOG_RING],
				"[%s, audio thread] %s", inst->plugin->desc->id,
				inst->log_line[t % LOG_RING]);
	atomic_store_explicit(&inst->log_tail, t, memory_order_release);
}

static const clap_host_log_t host_log_ext = { host_log };

/* host: thread-check */

static bool host_is_main_thread(const clap_host_t *host)
{
	struct cph_instance *inst = host->host_data;
	return pthread_equal(pthread_self(), inst->main_thread) && !tls_audio;
}

static bool host_is_audio_thread(const clap_host_t *host)
{
	(void)host;
	return tls_audio > 0;
}

static const clap_host_thread_check_t host_thread_check_ext = {
	host_is_main_thread, host_is_audio_thread,
};

/* host: params */

static void host_params_rescan(const clap_host_t *host, clap_param_rescan_flags flags)
{
	struct cph_instance *inst = host->host_data;
	if (flags & (CLAP_PARAM_RESCAN_ALL | CLAP_PARAM_RESCAN_INFO))
		cph_logf(&inst->log, CPH_LOG_WARN, "%s changed its parameter list; "
				"reload the filter to see new parameters",
				inst->plugin->desc->id);
}

static void host_params_clear(const clap_host_t *host, clap_id id, clap_param_clear_flags flags)
{
	(void)host; (void)id; (void)flags;
}

static void host_params_request_flush(const clap_host_t *host)
{
	struct cph_instance *inst = host->host_data;
	atomic_store(&inst->flush_req, 1);
	schedule_main(inst);
}

static const clap_host_params_t host_params_ext = {
	host_params_rescan, host_params_clear, host_params_request_flush,
};

/* host: latency */

static void host_latency_changed(const clap_host_t *host)
{
	/* re-read after every activate */
	(void)host;
}

static const clap_host_latency_t host_latency_ext = { host_latency_changed };

/* host: state */

static void host_state_mark_dirty(const clap_host_t *host)
{
	struct cph_instance *inst = host->host_data;
	inst->state_dirty = true;
}

static const clap_host_state_t host_state_ext = { host_state_mark_dirty };

/* host: audio-ports */

static bool host_audio_ports_is_rescan_flag_supported(const clap_host_t *host, uint32_t flag)
{
	(void)host;
	return flag == CLAP_AUDIO_PORTS_RESCAN_NAMES;
}

static void host_audio_ports_rescan(const clap_host_t *host, uint32_t flags)
{
	struct cph_instance *inst = host->host_data;
	if (flags & ~CLAP_AUDIO_PORTS_RESCAN_NAMES)
		cph_logf(&inst->log, CPH_LOG_WARN, "%s changed its audio ports; "
				"reload the filter to apply", inst->plugin->desc->id);
}

static const clap_host_audio_ports_t host_audio_ports_ext = {
	host_audio_ports_is_rescan_flag_supported, host_audio_ports_rescan,
};

static const void *host_get_extension(const clap_host_t *host, const char *id)
{
	(void)host;
	if (strcmp(id, CLAP_EXT_LOG) == 0)
		return &host_log_ext;
	if (strcmp(id, CLAP_EXT_THREAD_CHECK) == 0)
		return &host_thread_check_ext;
	if (strcmp(id, CLAP_EXT_PARAMS) == 0)
		return &host_params_ext;
	if (strcmp(id, CLAP_EXT_LATENCY) == 0)
		return &host_latency_ext;
	if (strcmp(id, CLAP_EXT_STATE) == 0)
		return &host_state_ext;
	if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
		return &host_audio_ports_ext;
	return NULL;
}

/* events */

static uint32_t in_events_size(const clap_input_events_t *list)
{
	const struct cph_instance *inst = list->ctx;
	return inst->n_ev_cur;
}

static const clap_event_header_t *in_events_get(const clap_input_events_t *list, uint32_t index)
{
	const struct cph_instance *inst = list->ctx;
	return index < inst->n_ev_cur ? &inst->ev[index].header : NULL;
}

static uint32_t empty_size(const clap_input_events_t *list)
{
	(void)list;
	return 0;
}

static const clap_event_header_t *empty_get(const clap_input_events_t *list, uint32_t index)
{
	(void)list; (void)index;
	return NULL;
}

static int map_cmp(const void *a, const void *b)
{
	const struct param_map *x = a, *y = b;
	return x->id < y->id ? -1 : x->id > y->id ? 1 : 0;
}

static const struct cph_port *port_by_param(const struct cph_instance *inst, clap_id id)
{
	struct param_map key = { .id = id }, *m;
	m = bsearch(&key, inst->map, inst->n_map, sizeof(*m), map_cmp);
	return m ? &inst->layout.ports[m->port] : NULL;
}

static bool out_events_try_push(const clap_output_events_t *list, const clap_event_header_t *ev)
{
	struct cph_instance *inst = list->ctx;
	const clap_event_param_value_t *pv;
	const struct cph_port *port;

	if (ev->space_id != CLAP_CORE_EVENT_SPACE_ID || ev->type != CLAP_EVENT_PARAM_VALUE)
		return true;
	pv = (const clap_event_param_value_t *)ev;
	if ((port = port_by_param(inst, pv->param_id)) == NULL)
		return true;
	if (port->kind == CPH_PORT_PARAM_OUT && inst->pout_ptr[port->slot])
		*inst->pout_ptr[port->slot] = (float)pv->value;
	return true;
}

/* layout discovery */

static int build_layout(struct cph_instance *inst)
{
	struct cph_layout *l = &inst->layout;
	const clap_plugin_t *plugin = inst->plugin;
	uint32_t i, c, n_params = 0, n_in_params = 0, n_out_params = 0, idx, pass;
	clap_audio_port_info_t api;
	clap_param_info_t pi;

	memset(l, 0, sizeof(*l));
	if (inst->audio_ports) {
		l->n_clap_in = inst->audio_ports->count(plugin, true);
		l->n_clap_out = inst->audio_ports->count(plugin, false);
	}
	l->clap_in_channels = calloc(l->n_clap_in ? l->n_clap_in : 1, sizeof(uint32_t));
	l->clap_out_channels = calloc(l->n_clap_out ? l->n_clap_out : 1, sizeof(uint32_t));
	if (!l->clap_in_channels || !l->clap_out_channels)
		return -ENOMEM;
	for (i = 0; i < l->n_clap_in; i++) {
		if (!inst->audio_ports->get(plugin, i, true, &api))
			return -EIO;
		l->clap_in_channels[i] = api.channel_count;
		l->n_audio_in += api.channel_count;
	}
	for (i = 0; i < l->n_clap_out; i++) {
		if (!inst->audio_ports->get(plugin, i, false, &api))
			return -EIO;
		l->clap_out_channels[i] = api.channel_count;
		l->n_audio_out += api.channel_count;
	}
	if (l->n_audio_in > MAX_CHANNELS || l->n_audio_out > MAX_CHANNELS)
		return -E2BIG;

	if (inst->params)
		n_params = inst->params->count(plugin);
	for (i = 0; i < n_params; i++) {
		if (!inst->params->get_info(plugin, i, &pi))
			continue;
		if (pi.flags & CLAP_PARAM_IS_HIDDEN)
			continue;
		if (pi.flags & CLAP_PARAM_IS_READONLY)
			n_out_params++;
		else
			n_in_params++;
	}

	l->n_ports = l->n_audio_in + l->n_audio_out + n_in_params + n_out_params + 1;
	l->ports = calloc(l->n_ports, sizeof(struct cph_port));
	if (l->ports == NULL)
		return -ENOMEM;

	idx = 0;
	for (pass = 0; pass < 2; pass++) {
		uint32_t n_clap = pass ? l->n_clap_out : l->n_clap_in;
		uint32_t *ch = pass ? l->clap_out_channels : l->clap_in_channels;
		uint32_t slot = 0;
		for (i = 0; i < n_clap; i++) {
			for (c = 0; c < ch[i]; c++) {
				struct cph_port *p = &l->ports[idx++];
				p->kind = pass ? CPH_PORT_AUDIO_OUT : CPH_PORT_AUDIO_IN;
				p->slot = slot++;
				p->clap_port = i;
				p->channel = c;
				if (i == 0)
					snprintf(p->name, sizeof(p->name), "%s_%u", pass ? "out" : "in", c + 1);
				else
					snprintf(p->name, sizeof(p->name), "%s%u_%u", pass ? "out" : "in", i + 1, c + 1);
			}
		}
	}

	inst->first_param_in = idx;
	l->n_params_in = n_in_params;
	l->n_params_out = n_out_params;
	for (pass = 0; pass < 2; pass++) {
		uint32_t slot = 0;
		if (pass == 1)
			inst->first_param_out = idx;
		for (i = 0; i < n_params; i++) {
			struct cph_port *p;
			char base[CPH_NAME_MAX];
			bool ro;

			if (!inst->params->get_info(plugin, i, &pi))
				continue;
			if (pi.flags & CLAP_PARAM_IS_HIDDEN)
				continue;
			ro = (pi.flags & CLAP_PARAM_IS_READONLY) != 0;
			if (ro != (pass == 1))
				continue;
			if (idx >= l->n_ports - 1)
				return -EIO;	/* the plugin changed its mind mid-scan */

			p = &l->ports[idx];
			p->kind = ro ? CPH_PORT_PARAM_OUT : CPH_PORT_PARAM_IN;
			p->slot = slot++;
			p->param_id = pi.id;
			p->cookie = pi.cookie;
			p->clap_flags = pi.flags;
			p->min = pi.min_value;
			p->max = pi.max_value;
			p->def = pi.default_value;
			if (p->max < p->min) {
				double t = p->min;
				p->min = p->max;
				p->max = t;
			}
			if (pi.flags & CLAP_PARAM_IS_STEPPED)
				p->hints |= (p->min == 0.0 && p->max == 1.0) ?
					CPH_HINT_BOOLEAN : CPH_HINT_INTEGER;
			snprintf(p->label, sizeof(p->label), "%.*s", (int)sizeof(p->label) - 1, pi.name);
			snprintf(p->module, sizeof(p->module), "%.*s", (int)sizeof(p->module) - 1, pi.module);

			cph_sanitize_name(pi.name, base, sizeof(base));
			if (base[0] == '\0')
				snprintf(base, sizeof(base), "param_%u", pi.id);
			snprintf(p->name, sizeof(p->name), "%s", base);
			if (name_taken(l->ports, idx, p->name))
				snprintf(p->name, sizeof(p->name), "%.100s_%u", base, pi.id);
			idx++;
		}
	}
	/* get_info may fail on the second pass: count what was really added */
	l->n_params_in = inst->first_param_out - inst->first_param_in;
	l->n_params_out = idx - inst->first_param_out;

	inst->latency_port = idx;
	{
		struct cph_port *p = &l->ports[idx++];
		p->kind = CPH_PORT_LATENCY;
		snprintf(p->name, sizeof(p->name), "%s",
				name_taken(l->ports, idx - 1, "latency") ? "plugin_latency" : "latency");
		p->hints = CPH_HINT_INTEGER;
		p->max = 1 << 24;
	}
	l->n_ports = idx;
	return 0;
}

static int alloc_runtime(struct cph_instance *inst)
{
	struct cph_layout *l = &inst->layout;
	uint32_t i, n = l->n_params_in + l->n_params_out, k, ch;
	size_t frames = inst->opts.max_frames;

	inst->in_ptr = calloc(l->n_audio_in + 1, sizeof(float *));
	inst->out_ptr = calloc(l->n_audio_out + 1, sizeof(float *));
	inst->pin_ptr = calloc(l->n_params_in + 1, sizeof(float *));
	inst->pout_ptr = calloc(l->n_params_out + 1, sizeof(float *));
	inst->pin_last = calloc(l->n_params_in + 1, sizeof(float));
	inst->ev = calloc(l->n_params_in + 1, sizeof(clap_event_param_value_t));
	inst->ain = calloc(l->n_clap_in + 1, sizeof(clap_audio_buffer_t));
	inst->aout = calloc(l->n_clap_out + 1, sizeof(clap_audio_buffer_t));
	inst->ain_ch = calloc(l->n_audio_in + 1, sizeof(float *));
	inst->aout_ch = calloc(l->n_audio_out + 1, sizeof(float *));
	inst->scratch = calloc((l->n_audio_in + 1) * frames, sizeof(float));
	inst->silence = calloc(frames, sizeof(float));
	inst->discard = calloc(frames, sizeof(float));
	inst->map = calloc(n + 1, sizeof(struct param_map));
	if (!inst->in_ptr || !inst->out_ptr || !inst->pin_ptr || !inst->pout_ptr ||
	    !inst->pin_last || !inst->ev || !inst->ain || !inst->aout || !inst->ain_ch ||
	    !inst->aout_ch || !inst->scratch || !inst->silence || !inst->discard || !inst->map)
		return -ENOMEM;

	for (i = 0; i < l->n_params_in; i++)
		inst->pin_last[i] = NAN;

	for (i = 0, ch = 0; i < l->n_clap_in; i++) {
		inst->ain[i].data32 = &inst->ain_ch[ch];
		inst->ain[i].channel_count = l->clap_in_channels[i];
		ch += l->clap_in_channels[i];
	}
	for (i = 0, ch = 0; i < l->n_clap_out; i++) {
		inst->aout[i].data32 = &inst->aout_ch[ch];
		inst->aout[i].channel_count = l->clap_out_channels[i];
		ch += l->clap_out_channels[i];
	}

	for (i = 0, k = 0; i < l->n_ports; i++) {
		const struct cph_port *p = &l->ports[i];
		if (p->kind == CPH_PORT_PARAM_IN || p->kind == CPH_PORT_PARAM_OUT) {
			inst->map[k].id = p->param_id;
			inst->map[k].port = i;
			k++;
		}
	}
	inst->n_map = k;
	qsort(inst->map, inst->n_map, sizeof(struct param_map), map_cmp);

	inst->in_events.ctx = inst;
	inst->in_events.size = in_events_size;
	inst->in_events.get = in_events_get;
	inst->empty_events.ctx = inst;
	inst->empty_events.size = empty_size;
	inst->empty_events.get = empty_get;
	inst->out_events.ctx = inst;
	inst->out_events.try_push = out_events_try_push;
	return 0;
}

static void free_memory(struct cph_instance *inst)
{
	cph_layout_clear(&inst->layout);
	free(inst->in_ptr);
	free(inst->out_ptr);
	free(inst->pin_ptr);
	free(inst->pout_ptr);
	free(inst->pin_last);
	free(inst->ev);
	free(inst->ain);
	free(inst->aout);
	free(inst->ain_ch);
	free(inst->aout_ch);
	free(inst->scratch);
	free(inst->silence);
	free(inst->discard);
	free(inst->map);
	free(inst);
}

struct cph_instance *cph_instance_new(struct cph_bundle *b, const char *plugin_id,
		const struct cph_instance_opts *opts, const struct cph_layout *expect)
{
	struct cph_instance *inst;
	const clap_plugin_descriptor_t *d;
	int res;

	if ((d = cph_bundle_find(b, plugin_id)) == NULL) {
		cph_logf(opts ? opts->log : NULL, CPH_LOG_ERROR, "%s: no plugin '%s'",
				cph_bundle_path(b), plugin_id ? plugin_id : "");
		errno = ENOENT;
		return NULL;
	}

	inst = calloc(1, sizeof(*inst));
	if (inst == NULL)
		return NULL;
	if (opts)
		inst->opts = *opts;
	if (inst->opts.log)
		inst->log = *inst->opts.log;
	inst->opts.log = &inst->log;
	if (inst->opts.sample_rate <= 0.0)
		inst->opts.sample_rate = DEFAULT_RATE;
	if (inst->opts.max_frames == 0)
		inst->opts.max_frames = DEFAULT_MAX_FRAMES;
	inst->main_thread = pthread_self();
	inst->bundle = b;
	bundle_ref(b);

	inst->host.clap_version = (clap_version_t)CLAP_VERSION_INIT;
	inst->host.host_data = inst;
	inst->host.name = "clap-pipewire";
	inst->host.vendor = "clap-pipewire";
	inst->host.url = "https://github.com/manuaudio/clap-pipewire";
	inst->host.version = CPH_VERSION;
	inst->host.get_extension = host_get_extension;
	inst->host.request_restart = host_request_restart;
	inst->host.request_process = host_request_process;
	inst->host.request_callback = host_request_callback;

	inst->plugin = b->factory->create_plugin(b->factory, &inst->host, d->id);
	if (inst->plugin == NULL) {
		cph_logf(&inst->log, CPH_LOG_ERROR, "%s: create_plugin failed", d->id);
		res = -EIO;
		goto error;
	}
	if (!inst->plugin->init(inst->plugin)) {
		cph_logf(&inst->log, CPH_LOG_ERROR, "%s: init failed", d->id);
		inst->plugin->destroy(inst->plugin);
		inst->plugin = NULL;
		res = -EIO;
		goto error;
	}
	inst->params = inst->plugin->get_extension(inst->plugin, CLAP_EXT_PARAMS);
	inst->audio_ports = inst->plugin->get_extension(inst->plugin, CLAP_EXT_AUDIO_PORTS);
	inst->latency = inst->plugin->get_extension(inst->plugin, CLAP_EXT_LATENCY);
	inst->state = inst->plugin->get_extension(inst->plugin, CLAP_EXT_STATE);

	if ((res = build_layout(inst)) < 0) {
		cph_logf(&inst->log, CPH_LOG_ERROR, "%s: can't read ports/params: %s",
				d->id, strerror(-res));
		goto error;
	}
	if (inst->layout.n_audio_in == 0 && inst->layout.n_audio_out == 0) {
		cph_logf(&inst->log, CPH_LOG_ERROR, "%s: no audio ports", d->id);
		res = -ENOTSUP;
		goto error;
	}
	if (expect && cph_layout_compare(&inst->layout, expect) != 0) {
		cph_logf(&inst->log, CPH_LOG_ERROR, "%s: ports or parameters differ from "
				"the probed layout", d->id);
		res = -EPROTO;
		goto error;
	}
	if ((res = alloc_runtime(inst)) < 0)
		goto error;
	atomic_store(&inst->run, RUN_INACTIVE);
	return inst;

error:
	cph_instance_free(inst);
	errno = -res;
	return NULL;
}

void cph_instance_free(struct cph_instance *inst)
{
	if (inst == NULL)
		return;
	if (inst->plugin) {
		cph_instance_deactivate(inst);
		flush_log(inst);
		inst->plugin->destroy(inst->plugin);
		inst->plugin = NULL;
	}
	if (inst->bundle) {
		cph_bundle_unref(inst->bundle);
		inst->bundle = NULL;
	}
	if (atomic_load(&inst->main_pending))
		inst->zombie = true;	/* freed by the queued main callback */
	else
		free_memory(inst);
}

const struct cph_layout *cph_instance_layout(const struct cph_instance *inst)
{
	return &inst->layout;
}

const clap_plugin_descriptor_t *cph_instance_descriptor(const struct cph_instance *inst)
{
	return inst->plugin ? inst->plugin->desc : NULL;
}

const clap_plugin_t *cph_instance_plugin(const struct cph_instance *inst)
{
	return inst->plugin;
}

void cph_instance_connect(struct cph_instance *inst, uint32_t port, void *data)
{
	const struct cph_port *p;

	if (port >= inst->layout.n_ports)
		return;
	p = &inst->layout.ports[port];
	switch (p->kind) {
	case CPH_PORT_AUDIO_IN:
		inst->in_ptr[p->slot] = data;
		break;
	case CPH_PORT_AUDIO_OUT:
		inst->out_ptr[p->slot] = data;
		break;
	case CPH_PORT_PARAM_IN:
		inst->pin_ptr[p->slot] = data;
		break;
	case CPH_PORT_PARAM_OUT:
		inst->pout_ptr[p->slot] = data;
		break;
	case CPH_PORT_LATENCY:
		inst->latency_ptr = data;
		if (data)
			*inst->latency_ptr = (float)inst->latency_frames;
		break;
	}
}

static void update_latency(struct cph_instance *inst)
{
	inst->latency_frames = inst->latency ? inst->latency->get(inst->plugin) : 0;
	if (inst->latency_ptr)
		*inst->latency_ptr = (float)inst->latency_frames;
}

int cph_instance_activate(struct cph_instance *inst)
{
	const struct cph_layout *l = &inst->layout;
	uint32_t i;

	if (inst->plugin == NULL)
		return -EINVAL;
	if (inst->activated)
		return 0;
	if (!inst->plugin->activate(inst->plugin, inst->opts.sample_rate, 1, inst->opts.max_frames)) {
		cph_logf(&inst->log, CPH_LOG_ERROR, "%s: activate(%.0f, 1, %u) failed",
				inst->plugin->desc->id, inst->opts.sample_rate, inst->opts.max_frames);
		return -EIO;
	}
	inst->activated = true;
	update_latency(inst);

	/* Controls still at their default are not sent: the plugin (possibly
	 * restored from state) keeps its own value. Anything else is sent on
	 * the first block. */
	if (!inst->baseline_done) {
		for (i = 0; i < l->n_params_in; i++) {
			const struct cph_port *p = &l->ports[inst->first_param_in + i];
			const float *ptr = inst->pin_ptr[i];
			if (ptr && *ptr == (float)p->def)
				inst->pin_last[i] = *ptr;
		}
		inst->baseline_done = true;
	}
	atomic_store_explicit(&inst->run, RUN_ACTIVE, memory_order_release);
	return 0;
}

static void stop_on_audio(void *arg)
{
	struct cph_instance *inst = arg;
	tls_audio++;
	if (inst->processing) {
		inst->plugin->stop_processing(inst->plugin);
		inst->processing = false;
	}
	tls_audio--;
}

void cph_instance_deactivate(struct cph_instance *inst)
{
	if (!inst->activated)
		return;
	atomic_store_explicit(&inst->run, RUN_INACTIVE, memory_order_release);
	if (inst->processing) {
		if (inst->opts.run_on_audio)
			inst->opts.run_on_audio(inst->opts.data, stop_on_audio, inst);
		else
			stop_on_audio(inst);
	}
	inst->plugin->deactivate(inst->plugin);
	inst->activated = false;
}

uint32_t cph_instance_latency(const struct cph_instance *inst)
{
	return inst->latency_frames;
}

static inline float *in_buf(struct cph_instance *inst, uint32_t c)
{
	return inst->in_ptr[c] ? inst->in_ptr[c] : inst->silence;
}

static inline float *out_buf(struct cph_instance *inst, uint32_t c)
{
	return inst->out_ptr[c] ? inst->out_ptr[c] : inst->discard;
}

static void passthrough(struct cph_instance *inst, uint32_t off, uint32_t n)
{
	const struct cph_layout *l = &inst->layout;
	uint32_t c;

	for (c = 0; c < l->n_audio_out; c++) {
		float *o = inst->out_ptr[c];
		if (o == NULL)
			continue;
		if (c < l->n_audio_in && inst->in_ptr[c]) {
			if (inst->in_ptr[c] != o)
				memmove(o + off, inst->in_ptr[c] + off, n * sizeof(float));
		} else {
			memset(o + off, 0, n * sizeof(float));
		}
	}
}

/* Does in[0..n) overlap any output's chunk at the same offset? */
static bool aliased(struct cph_instance *inst, const float *in, uint32_t off, uint32_t n)
{
	uint32_t c;
	for (c = 0; c < inst->layout.n_audio_out; c++) {
		const float *o = inst->out_ptr[c];
		if (o && o + off < in + n && in < o + off + n)
			return true;
	}
	return false;
}

static void collect_events(struct cph_instance *inst)
{
	const struct cph_layout *l = &inst->layout;
	uint32_t i, n = 0;

	for (i = 0; i < l->n_params_in; i++) {
		const float *ptr = inst->pin_ptr[i];
		const struct cph_port *p;
		clap_event_param_value_t *ev;
		float v;

		if (ptr == NULL)
			continue;
		__atomic_load(ptr, &v, __ATOMIC_RELAXED);	/* written by the main thread */
		if (v == inst->pin_last[i] || isnan(v))
			continue;
		inst->pin_last[i] = v;
		p = &l->ports[inst->first_param_in + i];

		ev = &inst->ev[n++];
		ev->header.size = sizeof(*ev);
		ev->header.time = 0;
		ev->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		ev->header.type = CLAP_EVENT_PARAM_VALUE;
		ev->header.flags = 0;
		ev->param_id = p->param_id;
		ev->cookie = p->cookie;
		ev->note_id = -1;
		ev->port_index = -1;
		ev->channel = -1;
		ev->key = -1;
		ev->value = v < p->min ? p->min : v > p->max ? p->max : (double)v;
	}
	inst->n_ev = n;
}

void cph_instance_process(struct cph_instance *inst, uint32_t n_frames)
{
	const struct cph_layout *l = &inst->layout;
	clap_process_t proc;
	uint32_t off, c, chunk, max = inst->opts.max_frames;
	int run;

	tls_audio++;

	run = atomic_load_explicit(&inst->run, memory_order_acquire);
	if (run == RUN_STOP_REQUESTED) {
		if (inst->processing) {
			inst->plugin->stop_processing(inst->plugin);
			inst->processing = false;
		}
		atomic_store_explicit(&inst->run, RUN_STOPPED, memory_order_release);
		schedule_main(inst);
		run = RUN_STOPPED;
	}
	if (run != RUN_ACTIVE) {
		passthrough(inst, 0, n_frames);
		goto done;
	}
	if (!inst->processing) {
		if (!inst->plugin->start_processing(inst->plugin)) {
			atomic_fetch_add_explicit(&inst->process_errors, 1, memory_order_relaxed);
			passthrough(inst, 0, n_frames);
			goto done;
		}
		inst->processing = true;
	}

	collect_events(inst);

	memset(&proc, 0, sizeof(proc));
	proc.transport = NULL;
	proc.audio_inputs = inst->ain;
	proc.audio_outputs = inst->aout;
	proc.audio_inputs_count = l->n_clap_in;
	proc.audio_outputs_count = l->n_clap_out;
	proc.in_events = &inst->in_events;
	proc.out_events = &inst->out_events;

	for (off = 0; off < n_frames; off += chunk) {
		chunk = n_frames - off < max ? n_frames - off : max;

		for (c = 0; c < l->n_audio_in; c++) {
			float *in = in_buf(inst, c) + (inst->in_ptr[c] ? off : 0);
			if (inst->in_ptr[c] && aliased(inst, in, off, chunk)) {
				float *s = inst->scratch + (size_t)c * max;
				memcpy(s, in, chunk * sizeof(float));
				in = s;
			}
			inst->ain_ch[c] = in;
		}
		for (c = 0; c < l->n_audio_out; c++)
			inst->aout_ch[c] = out_buf(inst, c) + (inst->out_ptr[c] ? off : 0);

		inst->n_ev_cur = off == 0 ? inst->n_ev : 0;
		proc.steady_time = inst->steady;
		proc.frames_count = chunk;

		if (inst->plugin->process(inst->plugin, &proc) == CLAP_PROCESS_ERROR) {
			atomic_fetch_add_explicit(&inst->process_errors, 1, memory_order_relaxed);
			passthrough(inst, off, chunk);
		}
		inst->steady += chunk;
	}
	inst->n_ev_cur = 0;
	atomic_fetch_add_explicit(&inst->frames, n_frames, memory_order_relaxed);
done:
	tls_audio--;
}

void cph_instance_main_callback(struct cph_instance *inst)
{
	atomic_store(&inst->main_pending, 0);
	if (inst->zombie) {
		free_memory(inst);
		return;
	}
	if (inst->plugin == NULL)
		return;

	flush_log(inst);

	if (atomic_exchange(&inst->callback_req, 0))
		inst->plugin->on_main_thread(inst->plugin);

	if (atomic_exchange(&inst->flush_req, 0) && !inst->activated &&
	    inst->params && inst->params->flush)
		inst->params->flush(inst->plugin, &inst->empty_events, &inst->out_events);

	if (atomic_load(&inst->restart_req)) {
		int run = atomic_load_explicit(&inst->run, memory_order_acquire);
		if (!inst->activated) {
			atomic_store(&inst->restart_req, 0);
		} else if (run == RUN_ACTIVE) {
			int expected = RUN_ACTIVE;
			/* the audio thread stops the plugin and calls us back */
			atomic_compare_exchange_strong(&inst->run, &expected, RUN_STOP_REQUESTED);
		} else if (run == RUN_STOPPED) {
			atomic_store(&inst->restart_req, 0);
			inst->plugin->deactivate(inst->plugin);
			inst->activated = false;
			atomic_store(&inst->run, RUN_INACTIVE);
			if (cph_instance_activate(inst) == 0) {
				atomic_fetch_add(&inst->restarts, 1);
				cph_logf(&inst->log, CPH_LOG_INFO, "%s restarted, latency %u",
						inst->plugin->desc->id, inst->latency_frames);
			}
		}
	}
}

/* ---- state -------------------------------------------------------------- */

static int64_t ostream_write(const clap_ostream_t *stream, const void *buffer, uint64_t size)
{
	FILE *f = stream->ctx;
	return fwrite(buffer, 1, size, f) == size ? (int64_t)size : -1;
}

struct mem_reader {
	const uint8_t *data;
	size_t size, pos;
};

static int64_t istream_read(const clap_istream_t *stream, void *buffer, uint64_t size)
{
	struct mem_reader *r = stream->ctx;
	size_t n = r->size - r->pos;
	if (n > size)
		n = size;
	memcpy(buffer, r->data + r->pos, n);
	r->pos += n;
	return (int64_t)n;
}

int cph_instance_state_save(struct cph_instance *inst, const char *path)
{
	char tmp[PATH_MAX];
	clap_ostream_t os;
	FILE *f;
	bool ok;

	if (inst->state == NULL)
		return -ENOTSUP;
	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp))
		return -ENAMETOOLONG;
	if ((f = fopen(tmp, "wb")) == NULL)
		return -errno;
	os.ctx = f;
	os.write = ostream_write;
	ok = inst->state->save(inst->plugin, &os);
	if (fclose(f) != 0)
		ok = false;
	if (!ok) {
		unlink(tmp);
		return -EIO;
	}
	if (rename(tmp, path) < 0) {
		int res = -errno;
		unlink(tmp);
		return res;
	}
	inst->state_dirty = false;
	return 0;
}

int cph_instance_state_load(struct cph_instance *inst, const char *path)
{
	struct mem_reader r = { 0 };
	clap_istream_t is;
	uint8_t *data = NULL;
	long size;
	FILE *f;
	bool ok;

	if (inst->state == NULL)
		return -ENOTSUP;
	if ((f = fopen(path, "rb")) == NULL)
		return -errno;
	if (fseek(f, 0, SEEK_END) < 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) < 0) {
		fclose(f);
		return -EIO;
	}
	if ((data = malloc(size ? (size_t)size : 1)) == NULL) {
		fclose(f);
		return -ENOMEM;
	}
	if (fread(data, 1, (size_t)size, f) != (size_t)size) {
		free(data);
		fclose(f);
		return -EIO;
	}
	fclose(f);
	r.data = data;
	r.size = (size_t)size;
	is.ctx = &r;
	is.read = istream_read;
	ok = inst->state->load(inst->plugin, &is);
	free(data);
	return ok ? 0 : -EIO;
}

bool cph_instance_state_dirty(const struct cph_instance *inst)
{
	return inst->state_dirty;
}

int cph_instance_param_value(struct cph_instance *inst, uint32_t port, double *value)
{
	const struct cph_port *p;

	if (port >= inst->layout.n_ports || inst->params == NULL)
		return -EINVAL;
	p = &inst->layout.ports[port];
	if (p->kind != CPH_PORT_PARAM_IN && p->kind != CPH_PORT_PARAM_OUT)
		return -EINVAL;
	return inst->params->get_value(inst->plugin, p->param_id, value) ? 0 : -EIO;
}

int cph_instance_param_text(struct cph_instance *inst, uint32_t port, double value,
		char *buf, size_t size)
{
	const struct cph_port *p;

	if (port >= inst->layout.n_ports || inst->params == NULL || size == 0)
		return -EINVAL;
	p = &inst->layout.ports[port];
	if (p->kind != CPH_PORT_PARAM_IN && p->kind != CPH_PORT_PARAM_OUT)
		return -EINVAL;
	buf[0] = '\0';
	if (inst->params->value_to_text == NULL ||
	    !inst->params->value_to_text(inst->plugin, p->param_id, value, buf, (uint32_t)size))
		return -ENOTSUP;
	return 0;
}

int cph_instance_param_parse(struct cph_instance *inst, uint32_t port, const char *text,
		double *value)
{
	const struct cph_port *p;

	if (port >= inst->layout.n_ports || inst->params == NULL || text == NULL)
		return -EINVAL;
	p = &inst->layout.ports[port];
	if (p->kind != CPH_PORT_PARAM_IN && p->kind != CPH_PORT_PARAM_OUT)
		return -EINVAL;
	if (inst->params->text_to_value == NULL ||
	    !inst->params->text_to_value(inst->plugin, p->param_id, text, value))
		return -EINVAL;
	return 0;
}

int cph_instance_param_set(struct cph_instance *inst, uint32_t port, double value)
{
	const struct cph_port *p;
	clap_event_param_value_t *ev = &inst->ev[0];

	if (port >= inst->layout.n_ports || inst->params == NULL || inst->params->flush == NULL)
		return -EINVAL;
	if (inst->activated)
		return -EBUSY;
	p = &inst->layout.ports[port];
	if (p->kind != CPH_PORT_PARAM_IN)
		return -EINVAL;

	memset(ev, 0, sizeof(*ev));
	ev->header.size = sizeof(*ev);
	ev->header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	ev->header.type = CLAP_EVENT_PARAM_VALUE;
	ev->param_id = p->param_id;
	ev->cookie = p->cookie;
	ev->note_id = -1;
	ev->port_index = -1;
	ev->channel = -1;
	ev->key = -1;
	ev->value = value < p->min ? p->min : value > p->max ? p->max : value;
	/* not active, so the audio side is idle and owns nothing */
	inst->n_ev_cur = 1;
	inst->params->flush(inst->plugin, &inst->in_events, &inst->out_events);
	inst->n_ev_cur = 0;
	return 0;
}

int cph_layout_find(const struct cph_layout *l, const char *name)
{
	uint32_t i;
	for (i = 0; i < l->n_ports; i++)
		if (strcmp(l->ports[i].name, name) == 0)
			return (int)i;
	for (i = 0; i < l->n_ports; i++)
		if ((l->ports[i].kind == CPH_PORT_PARAM_IN || l->ports[i].kind == CPH_PORT_PARAM_OUT) &&
		    strcmp(l->ports[i].label, name) == 0)
			return (int)i;
	return -1;
}

void cph_instance_stats(const struct cph_instance *inst, struct cph_stats *stats)
{
	stats->process_errors = atomic_load(&inst->process_errors);
	stats->log_dropped = atomic_load(&inst->log_dropped);
	stats->restarts = atomic_load(&inst->restarts);
	stats->frames = atomic_load(&inst->frames);
}

int cph_probe_layout(struct cph_bundle *b, const char *plugin_id, double sample_rate,
		const struct cph_log *log, struct cph_layout *out)
{
	struct cph_instance_opts opts = {
		.sample_rate = sample_rate,
		.log = log,
	};
	struct cph_instance *inst;
	int res;

	if ((inst = cph_instance_new(b, plugin_id, &opts, NULL)) == NULL)
		return errno ? -errno : -EIO;
	res = cph_layout_copy(out, &inst->layout);
	cph_instance_free(inst);
	return res;
}
