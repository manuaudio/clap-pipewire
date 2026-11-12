/* clap-pipewire: a tiny CLAP effect used by the tests */
/* SPDX-FileCopyrightText: Copyright © 2026 manuaudio */
/* SPDX-License-Identifier: MIT */

/*
 * Two plugins:
 *   org.clap-pipewire.test.gain  stereo gain
 *   org.clap-pipewire.test.mono  the same, mono
 *
 * Parameters (ids):
 *   1 "Gain (dB)"  -60..12, default 0
 *   2 "Mute"       stepped 0..1
 *   3 "Mode"       stepped 0..3
 *   4 "Latency"    stepped 0..512, applied after a host restart
 *   5 "Peak"       read-only, the block's output peak, sent as an output event
 *   6 "Secret"     hidden
 *   7 "Gain (dB)"  a second param with the same name (name clash)
 *
 * It checks the host keeps CLAP's thread and lifecycle rules and counts
 * violations, exposed with everything else through TEST_EXT_ID.
 */

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <clap/clap.h>

#include "test-plugin.h"

struct tp {
	clap_plugin_t plugin;
	const clap_host_t *host;
	const clap_host_thread_check_t *tc;
	const clap_host_log_t *log;
	const clap_host_latency_t *host_latency;
	uint32_t channels;

	double gain_db, mute, mode, latency, latency_applied, secret, gain2;
	double sample_rate;
	uint32_t max_frames;
	bool active, processing;

	atomic_uint violations;
	atomic_uint param_events;
	atomic_uint main_callbacks;
	atomic_uint process_calls;
	uint32_t last_frames;
	uint32_t max_seen_frames;
	bool log_from_audio;
};

static void violation(struct tp *t, const char *what)
{
	atomic_fetch_add(&t->violations, 1);
	fprintf(stderr, "test-plugin: VIOLATION: %s\n", what);
}

static void check_main(struct tp *t, const char *what)
{
	if (t->tc && !t->tc->is_main_thread(t->host))
		violation(t, what);
}

static void check_audio(struct tp *t, const char *what)
{
	if (t->tc && !t->tc->is_audio_thread(t->host))
		violation(t, what);
}

static double *param_ptr(struct tp *t, clap_id id)
{
	switch (id) {
	case 1: return &t->gain_db;
	case 2: return &t->mute;
	case 3: return &t->mode;
	case 4: return &t->latency;
	case 6: return &t->secret;
	case 7: return &t->gain2;
	}
	return NULL;
}

/* params */

static uint32_t params_count(const clap_plugin_t *p)
{
	return 7;
}

static bool params_get_info(const clap_plugin_t *p, uint32_t index, clap_param_info_t *info)
{
	static const struct {
		clap_id id;
		const char *name;
		uint32_t flags;
		double min, max, def;
	} tab[] = {
		{ 1, "Gain (dB)", CLAP_PARAM_IS_AUTOMATABLE, -60, 12, 0 },
		{ 2, "Mute", CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE, 0, 1, 0 },
		{ 3, "Mode", CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM, 0, 3, 1 },
		{ 4, "Latency", CLAP_PARAM_IS_STEPPED, 0, 512, 0 },
		{ 5, "Peak", CLAP_PARAM_IS_READONLY, 0, 4, 0 },
		{ 6, "Secret", CLAP_PARAM_IS_HIDDEN, 0, 1, 0 },
		{ 7, "Gain (dB)", 0, -10, 10, 0 },
	};
	struct tp *t = p->plugin_data;

	check_main(t, "params.get_info off the main thread");
	if (index >= sizeof(tab) / sizeof(tab[0]))
		return false;
	memset(info, 0, sizeof(*info));
	info->id = tab[index].id;
	info->flags = tab[index].flags;
	info->cookie = param_ptr(t, tab[index].id);
	snprintf(info->name, sizeof(info->name), "%s", tab[index].name);
	snprintf(info->module, sizeof(info->module), "%s", index < 4 ? "Main" : "");
	info->min_value = tab[index].min;
	info->max_value = tab[index].max;
	info->default_value = tab[index].def;
	return true;
}

static bool params_get_value(const clap_plugin_t *p, clap_id id, double *value)
{
	struct tp *t = p->plugin_data;
	double *v = param_ptr(t, id);
	if (id == 5) {
		*value = 0;
		return true;
	}
	if (v == NULL)
		return false;
	*value = *v;
	return true;
}

static bool params_value_to_text(const clap_plugin_t *p, clap_id id, double value,
		char *out, uint32_t size)
{
	static const char *modes[] = { "Clean", "Warm", "Hot", "Broken" };
	if (id == 1 || id == 7)
		snprintf(out, size, "%.2f dB", value);
	else if (id == 3 && value >= 0 && value <= 3)
		snprintf(out, size, "%s", modes[(int)value]);
	else
		snprintf(out, size, "%g", value);
	return true;
}

static bool params_text_to_value(const clap_plugin_t *p, clap_id id, const char *text, double *value)
{
	static const char *modes[] = { "Clean", "Warm", "Hot", "Broken" };
	char *end;
	int i;

	if (id == 3) {
		for (i = 0; i < 4; i++)
			if (strcmp(text, modes[i]) == 0) {
				*value = i;
				return true;
			}
	}
	*value = strtod(text, &end);
	return end != text;
}

static void apply_event(struct tp *t, const clap_event_header_t *h)
{
	const clap_event_param_value_t *ev;
	double *v;

	if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE)
		return;
	ev = (const clap_event_param_value_t *)h;
	if ((v = param_ptr(t, ev->param_id)) == NULL)
		return;
	if (ev->cookie != v)
		violation(t, "wrong cookie");
	if (ev->note_id != -1 || ev->port_index != -1 || ev->channel != -1 || ev->key != -1)
		violation(t, "param event is not global");
	*v = ev->value;
	atomic_fetch_add(&t->param_events, 1);
	if (ev->param_id == 4 && t->latency != t->latency_applied)
		t->host->request_restart(t->host);
}

static void params_flush(const clap_plugin_t *p, const clap_input_events_t *in,
		const clap_output_events_t *out)
{
	struct tp *t = p->plugin_data;
	uint32_t i, n = in->size(in);

	if (t->active)
		check_audio(t, "params.flush while active, off the audio thread");
	else
		check_main(t, "params.flush while inactive, off the main thread");
	for (i = 0; i < n; i++)
		apply_event(t, in->get(in, i));
}

static const clap_plugin_params_t params_ext = {
	params_count, params_get_info, params_get_value,
	params_value_to_text, params_text_to_value, params_flush,
};

/* audio-ports */

static uint32_t ap_count(const clap_plugin_t *p, bool is_input)
{
	return 1;
}

static bool ap_get(const clap_plugin_t *p, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
	struct tp *t = p->plugin_data;
	if (index != 0)
		return false;
	memset(info, 0, sizeof(*info));
	info->id = is_input ? 0 : 1;
	snprintf(info->name, sizeof(info->name), "%s", is_input ? "In" : "Out");
	info->flags = CLAP_AUDIO_PORT_IS_MAIN;
	info->channel_count = t->channels;
	info->port_type = t->channels == 2 ? CLAP_PORT_STEREO : CLAP_PORT_MONO;
	info->in_place_pair = CLAP_INVALID_ID;
	return true;
}

static const clap_plugin_audio_ports_t audio_ports_ext = { ap_count, ap_get };

/* latency */

static uint32_t latency_get(const clap_plugin_t *p)
{
	struct tp *t = p->plugin_data;
	return (uint32_t)t->latency_applied;
}

static const clap_plugin_latency_t latency_ext = { latency_get };

/* state: "gain mute mode latency" as text */

static bool state_save(const clap_plugin_t *p, const clap_ostream_t *stream)
{
	struct tp *t = p->plugin_data;
	char buf[256];
	int n;

	check_main(t, "state.save off the main thread");
	n = snprintf(buf, sizeof(buf), "test-plugin-state 1 %.17g %.17g %.17g %.17g\n",
			t->gain_db, t->mute, t->mode, t->latency);
	return stream->write(stream, buf, (uint64_t)n) == n;
}

static bool state_load(const clap_plugin_t *p, const clap_istream_t *stream)
{
	struct tp *t = p->plugin_data;
	char buf[256];
	int64_t n, total = 0;

	check_main(t, "state.load off the main thread");
	while (total < (int64_t)sizeof(buf) - 1 &&
	       (n = stream->read(stream, buf + total, 7)) > 0)	/* odd chunks on purpose */
		total += n;
	buf[total] = '\0';
	if (sscanf(buf, "test-plugin-state 1 %lg %lg %lg %lg", &t->gain_db, &t->mute,
				&t->mode, &t->latency) != 4)
		return false;
	return true;
}

static const clap_plugin_state_t state_ext = { state_save, state_load };

/* test extension */

static uint32_t te_violations(const clap_plugin_t *p)
{
	return atomic_load(&((struct tp *)p->plugin_data)->violations);
}

static uint32_t te_param_events(const clap_plugin_t *p)
{
	return atomic_load(&((struct tp *)p->plugin_data)->param_events);
}

static uint32_t te_main_callbacks(const clap_plugin_t *p)
{
	return atomic_load(&((struct tp *)p->plugin_data)->main_callbacks);
}

static uint32_t te_process_calls(const clap_plugin_t *p)
{
	return atomic_load(&((struct tp *)p->plugin_data)->process_calls);
}

static uint32_t te_max_frames_seen(const clap_plugin_t *p)
{
	return ((struct tp *)p->plugin_data)->max_seen_frames;
}

static double te_value(const clap_plugin_t *p, clap_id id)
{
	struct tp *t = p->plugin_data;
	double *v = param_ptr(t, id);
	return v ? *v : NAN;
}

static void te_request_callback(const clap_plugin_t *p)
{
	struct tp *t = p->plugin_data;
	t->host->request_callback(t->host);
}

static void te_log_from_audio(const clap_plugin_t *p, bool on)
{
	((struct tp *)p->plugin_data)->log_from_audio = on;
}

static const struct test_plugin_ext test_ext = {
	te_violations, te_param_events, te_main_callbacks, te_process_calls,
	te_max_frames_seen, te_value, te_request_callback, te_log_from_audio,
};

/* plugin */

static bool tp_init(const clap_plugin_t *p)
{
	struct tp *t = p->plugin_data;
	t->tc = t->host->get_extension(t->host, CLAP_EXT_THREAD_CHECK);
	t->log = t->host->get_extension(t->host, CLAP_EXT_LOG);
	t->host_latency = t->host->get_extension(t->host, CLAP_EXT_LATENCY);
	if (!t->tc || !t->log || !t->host_latency ||
	    !t->host->get_extension(t->host, CLAP_EXT_PARAMS) ||
	    !t->host->get_extension(t->host, CLAP_EXT_STATE))
		violation(t, "host lacks a basic extension");
	check_main(t, "init off the main thread");
	t->mode = 1;
	return true;
}

static void tp_destroy(const clap_plugin_t *p)
{
	struct tp *t = p->plugin_data;
	check_main(t, "destroy off the main thread");
	if (t->active)
		violation(t, "destroyed while active");
	free(t);
}

static bool tp_activate(const clap_plugin_t *p, double sr, uint32_t min, uint32_t max)
{
	struct tp *t = p->plugin_data;
	check_main(t, "activate off the main thread");
	if (t->active)
		violation(t, "activate twice");
	if (min < 1 || max < min)
		violation(t, "bad frame counts");
	t->sample_rate = sr;
	t->max_frames = max;
	t->latency_applied = t->latency;
	t->active = true;
	return true;
}

static void tp_deactivate(const clap_plugin_t *p)
{
	struct tp *t = p->plugin_data;
	check_main(t, "deactivate off the main thread");
	if (!t->active)
		violation(t, "deactivate while inactive");
	if (t->processing)
		violation(t, "deactivate while processing");
	t->active = false;
}

static bool tp_start_processing(const clap_plugin_t *p)
{
	struct tp *t = p->plugin_data;
	check_audio(t, "start_processing off the audio thread");
	if (!t->active || t->processing)
		violation(t, "start_processing in the wrong state");
	t->processing = true;
	return true;
}

static void tp_stop_processing(const clap_plugin_t *p)
{
	struct tp *t = p->plugin_data;
	check_audio(t, "stop_processing off the audio thread");
	if (!t->processing)
		violation(t, "stop_processing while not processing");
	t->processing = false;
}

static void tp_reset(const clap_plugin_t *p)
{
}

static clap_process_status tp_process(const clap_plugin_t *p, const clap_process_t *proc)
{
	struct tp *t = p->plugin_data;
	uint32_t i, n, c, ch = t->channels;
	double before = t->gain_db;
	float g, peak = 0.0f;

	check_audio(t, "process off the audio thread");
	if (!t->processing)
		violation(t, "process while not processing");
	if (proc->frames_count > t->max_frames || proc->frames_count == 0)
		violation(t, "frames_count out of range");
	if (proc->audio_inputs_count != 1 || proc->audio_outputs_count != 1 ||
	    proc->audio_inputs[0].channel_count != ch || proc->audio_outputs[0].channel_count != ch)
		violation(t, "wrong buffers");
	atomic_fetch_add(&t->process_calls, 1);
	if (proc->frames_count > t->max_seen_frames)
		t->max_seen_frames = proc->frames_count;

	n = proc->in_events->size(proc->in_events);
	for (i = 0; i < n; i++)
		apply_event(t, proc->in_events->get(proc->in_events, i));

	g = t->mute >= 0.5 ? 0.0f : powf(10.0f, (float)t->gain_db / 20.0f);
	for (c = 0; c < ch; c++) {
		const float *in = proc->audio_inputs[0].data32[c];
		float *out = proc->audio_outputs[0].data32[c];
		if (in == out)
			violation(t, "in-place buffers without in_place_pair");
		for (i = 0; i < proc->frames_count; i++) {
			out[i] = in[i] * g;
			if (fabsf(out[i]) > peak)
				peak = fabsf(out[i]);
		}
	}

	{
		clap_event_param_value_t ev = {
			.header = { sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 },
			.param_id = 5, .note_id = -1, .port_index = -1, .channel = -1, .key = -1,
			.value = peak,
		};
		proc->out_events->try_push(proc->out_events, &ev.header);
	}
	if (t->log_from_audio && before != t->gain_db)
		t->log->log(t->host, CLAP_LOG_INFO, "gain changed on the audio thread");
	return CLAP_PROCESS_CONTINUE;
}

static const void *tp_get_extension(const clap_plugin_t *p, const char *id)
{
	if (strcmp(id, CLAP_EXT_PARAMS) == 0)
		return &params_ext;
	if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
		return &audio_ports_ext;
	if (strcmp(id, CLAP_EXT_LATENCY) == 0)
		return &latency_ext;
	if (strcmp(id, CLAP_EXT_STATE) == 0)
		return &state_ext;
	if (strcmp(id, TEST_EXT_ID) == 0)
		return &test_ext;
	return NULL;
}

static void tp_on_main_thread(const clap_plugin_t *p)
{
	struct tp *t = p->plugin_data;
	check_main(t, "on_main_thread off the main thread");
	atomic_fetch_add(&t->main_callbacks, 1);
}

static const char *features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, NULL };

static const clap_plugin_descriptor_t desc_gain = {
	.clap_version = CLAP_VERSION_INIT,
	.id = "org.clap-pipewire.test.gain",
	.name = "Test Gain",
	.vendor = "clap-pipewire",
	.version = "1.0.0",
	.description = "Stereo gain for the clap-pipewire tests",
	.features = features,
};

static const clap_plugin_descriptor_t desc_mono = {
	.clap_version = CLAP_VERSION_INIT,
	.id = "org.clap-pipewire.test.mono",
	.name = "Test Mono Gain",
	.vendor = "clap-pipewire",
	.version = "1.0.0",
	.description = "Mono gain for the clap-pipewire tests",
	.features = features,
};

static uint32_t f_count(const clap_plugin_factory_t *f)
{
	return 2;
}

static const clap_plugin_descriptor_t *f_desc(const clap_plugin_factory_t *f, uint32_t index)
{
	return index == 0 ? &desc_gain : index == 1 ? &desc_mono : NULL;
}

static const clap_plugin_t *f_create(const clap_plugin_factory_t *f, const clap_host_t *host,
		const char *id)
{
	struct tp *t;
	const clap_plugin_descriptor_t *d;

	if (strcmp(id, desc_gain.id) == 0)
		d = &desc_gain;
	else if (strcmp(id, desc_mono.id) == 0)
		d = &desc_mono;
	else
		return NULL;
	if ((t = calloc(1, sizeof(*t))) == NULL)
		return NULL;
	t->host = host;
	t->channels = d == &desc_gain ? 2 : 1;
	t->plugin.desc = d;
	t->plugin.plugin_data = t;
	t->plugin.init = tp_init;
	t->plugin.destroy = tp_destroy;
	t->plugin.activate = tp_activate;
	t->plugin.deactivate = tp_deactivate;
	t->plugin.start_processing = tp_start_processing;
	t->plugin.stop_processing = tp_stop_processing;
	t->plugin.reset = tp_reset;
	t->plugin.process = tp_process;
	t->plugin.get_extension = tp_get_extension;
	t->plugin.on_main_thread = tp_on_main_thread;
	return &t->plugin;
}

static const clap_plugin_factory_t factory = { f_count, f_desc, f_create };

static int init_count;

static bool entry_init(const char *path)
{
	init_count++;
	return true;
}

static void entry_deinit(void)
{
	if (--init_count < 0)
		fprintf(stderr, "test-plugin: VIOLATION: deinit without init\n");
}

static const void *entry_get_factory(const char *id)
{
	return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
	CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory,
};
