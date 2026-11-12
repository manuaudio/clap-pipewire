/* clap-pipewire: unit tests for the CLAP host core */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "clap-host.h"
#include "test-plugin/test-plugin.h"
#include "check.h"

#define GAIN "org.clap-pipewire.test.gain"
#define MONO "org.clap-pipewire.test.mono"

static const char *plugin_path = TEST_PLUGIN_PATH;
static char log_buf[8192];

static void log_cb(void *data, enum cph_log_level level, const char *msg)
{
	size_t n = strlen(log_buf);
	snprintf(log_buf + n, sizeof(log_buf) - n, "%s\n", msg);
	if (getenv("TEST_VERBOSE"))
		fprintf(stderr, "log: %s\n", msg);
}
static const struct cph_log logger = { log_cb, NULL };

/* a fake main loop: requests are recorded, the test drains them */
static struct cph_instance *pending[16];
static int n_pending;

static int request_main(void *data, struct cph_instance *inst)
{
	if (n_pending >= 16)
		return -ENOSPC;
	pending[n_pending++] = inst;
	return 0;
}

static int drain_main(void)
{
	int i, n = n_pending;
	n_pending = 0;
	for (i = 0; i < n; i++)
		cph_instance_main_callback(pending[i]);
	return n;
}

struct rig {
	struct cph_bundle *b;
	struct cph_instance *inst;
	const struct test_plugin_ext *ext;
	const clap_plugin_t *plugin;
	float in[2][1024], out[2][1024];
	float ctl[16], notify[16], latency;
};

static int port_of(struct rig *r, const char *name)
{
	return cph_layout_find(cph_instance_layout(r->inst), name);
}

static void rig_open(struct rig *r, const char *id, uint32_t max_frames)
{
	struct cph_instance_opts opts = {
		.sample_rate = 48000, .max_frames = max_frames, .log = &logger,
		.request_main = request_main,
	};
	const struct cph_layout *l;
	uint32_t i;

	memset(r, 0, sizeof(*r));
	r->b = cph_bundle_open(plugin_path, &logger);
	CHECK(r->b != NULL);
	r->inst = cph_instance_new(r->b, id, &opts, NULL);
	CHECK(r->inst != NULL);
	r->plugin = cph_instance_plugin(r->inst);
	r->ext = r->plugin->get_extension(r->plugin, TEST_EXT_ID);
	CHECK(r->ext != NULL);
	l = cph_instance_layout(r->inst);
	for (i = 0; i < l->n_ports; i++) {
		const struct cph_port *p = &l->ports[i];
		switch (p->kind) {
		case CPH_PORT_AUDIO_IN: cph_instance_connect(r->inst, i, r->in[p->slot]); break;
		case CPH_PORT_AUDIO_OUT: cph_instance_connect(r->inst, i, r->out[p->slot]); break;
		case CPH_PORT_PARAM_IN:
			r->ctl[p->slot] = (float)p->def;
			cph_instance_connect(r->inst, i, &r->ctl[p->slot]);
			break;
		case CPH_PORT_PARAM_OUT: cph_instance_connect(r->inst, i, &r->notify[p->slot]); break;
		case CPH_PORT_LATENCY: cph_instance_connect(r->inst, i, &r->latency); break;
		}
	}
}

static float *ctl(struct rig *r, const char *name)
{
	int port = port_of(r, name);
	CHECK(port >= 0);
	return &r->ctl[cph_instance_layout(r->inst)->ports[port].slot];
}

static void fill(struct rig *r, float v, uint32_t n)
{
	uint32_t c, i;
	for (c = 0; c < 2; c++)
		for (i = 0; i < n; i++) {
			r->in[c][i] = v;
			r->out[c][i] = -99.0f;
		}
}

static void rig_close(struct rig *r)
{
	CHECK_EQ_U(r->ext->violations(r->plugin), 0);
	cph_instance_free(r->inst);
	cph_bundle_unref(r->b);
	drain_main();
}

/* ---- tests -------------------------------------------------------------- */

static void test_sanitize(void)
{
	char out[64];
	cph_sanitize_name("Gain (dB)", out, sizeof(out));
	CHECK_STR(out, "Gain_dB");
	cph_sanitize_name("  a::b  ", out, sizeof(out));
	CHECK_STR(out, "a_b");
	cph_sanitize_name("Filter.Freq-2", out, sizeof(out));
	CHECK_STR(out, "Filter.Freq-2");
	cph_sanitize_name("***", out, sizeof(out));
	CHECK_STR(out, "");
}

static void test_resolve(void)
{
	char dir[] = "/tmp/clap-pw-test-XXXXXX", p[PATH_MAX], q[PATH_MAX], path[PATH_MAX];
	char real[PATH_MAX];

	CHECK(mkdtemp(dir) != NULL);
	CHECK(realpath(plugin_path, real) != NULL);
	snprintf(p, sizeof(p), "%s/tp.clap", dir);
	CHECK(symlink(real, p) == 0);
	snprintf(p, sizeof(p), "%s/Dir.clap", dir);
	CHECK(mkdir(p, 0755) == 0);
	snprintf(q, sizeof(q), "%s/Dir.clap/Dir.clap", dir);
	CHECK(symlink(real, q) == 0);
	setenv("CLAP_PATH", dir, 1);

	CHECK(cph_search_path(path, sizeof(path)) == 0);
	CHECK(strncmp(path, dir, strlen(dir)) == 0);
	CHECK(strstr(path, "/.clap:") != NULL);

	CHECK(cph_resolve("tp.clap", path, sizeof(path)) == 0);
	snprintf(p, sizeof(p), "%s/tp.clap", dir);
	CHECK_STR(path, p);
	CHECK(cph_resolve("Dir.clap", path, sizeof(path)) == 0);
	CHECK_STR(path, q);
	CHECK(cph_resolve("Dir.clap/Dir.clap", path, sizeof(path)) == 0);
	CHECK(cph_resolve("missing.clap", path, sizeof(path)) == -ENOENT);
	CHECK(cph_resolve("../etc/passwd", path, sizeof(path)) == -EINVAL);
	CHECK(cph_resolve(real, path, sizeof(path)) == 0);
	CHECK(cph_resolve("", path, sizeof(path)) == -EINVAL);

	unlink(q);
	snprintf(p, sizeof(p), "%s/Dir.clap", dir);
	rmdir(p);
	snprintf(p, sizeof(p), "%s/tp.clap", dir);
	unlink(p);
	rmdir(dir);
	unsetenv("CLAP_PATH");
}

static void test_bundle(void)
{
	struct cph_bundle *a = cph_bundle_open(plugin_path, &logger);
	struct cph_bundle *b = cph_bundle_open(plugin_path, &logger);

	CHECK(a != NULL && a == b);
	CHECK_EQ_U(cph_bundle_n_plugins(a), 2);
	CHECK_STR(cph_bundle_find(a, GAIN)->id, GAIN);
	CHECK_STR(cph_bundle_find(a, "test mono gain")->id, MONO);
	CHECK(cph_bundle_find(a, "") == NULL);	/* two plugins: must say which */
	CHECK(cph_bundle_find(a, "nope") == NULL);
	cph_bundle_unref(b);
	cph_bundle_unref(a);
	CHECK(cph_bundle_open("/nonexistent.clap", &logger) == NULL);
}

static void test_layout(void)
{
	struct rig r;
	const struct cph_layout *l;
	struct cph_layout probe = { 0 };
	int i;

	rig_open(&r, GAIN, 0);
	l = cph_instance_layout(r.inst);
	CHECK_EQ_U(l->n_audio_in, 2);
	CHECK_EQ_U(l->n_audio_out, 2);
	CHECK_EQ_U(l->n_params_in, 5);
	CHECK_EQ_U(l->n_params_out, 1);
	CHECK_EQ_U(l->n_ports, 2 + 2 + 5 + 1 + 1);

	static const char *names[] = { "in_1", "in_2", "out_1", "out_2", "Gain_dB", "Mute",
		"Mode", "Latency", "Gain_dB_7", "Peak", "latency" };
	static const enum cph_port_kind kinds[] = { CPH_PORT_AUDIO_IN, CPH_PORT_AUDIO_IN,
		CPH_PORT_AUDIO_OUT, CPH_PORT_AUDIO_OUT, CPH_PORT_PARAM_IN, CPH_PORT_PARAM_IN,
		CPH_PORT_PARAM_IN, CPH_PORT_PARAM_IN, CPH_PORT_PARAM_IN, CPH_PORT_PARAM_OUT,
		CPH_PORT_LATENCY };
	for (i = 0; i < 11; i++) {
		CHECK_STR(l->ports[i].name, names[i]);
		CHECK(l->ports[i].kind == kinds[i]);
	}
	CHECK(port_of(&r, "Secret") < 0);			/* hidden */
	CHECK(l->ports[port_of(&r, "Mute")].hints & CPH_HINT_BOOLEAN);
	CHECK(l->ports[port_of(&r, "Mode")].hints & CPH_HINT_INTEGER);
	CHECK(l->ports[port_of(&r, "Gain_dB")].min == -60.0);
	CHECK(l->ports[port_of(&r, "Gain_dB")].max == 12.0);
	CHECK_EQ_I(port_of(&r, "Gain (dB)"), port_of(&r, "Gain_dB"));	/* by CLAP name */
	CHECK_STR(l->ports[port_of(&r, "Mode")].module, "Main");

	CHECK(cph_probe_layout(r.b, GAIN, 44100, &logger, &probe) == 0);
	CHECK(cph_layout_compare(&probe, l) == 0);
	probe.ports[4].param_id = 99;
	CHECK(cph_layout_compare(&probe, l) != 0);
	cph_layout_clear(&probe);
	CHECK(cph_probe_layout(r.b, MONO, 44100, &logger, &probe) == 0);
	CHECK_EQ_U(probe.n_audio_in, 1);
	CHECK_EQ_U(probe.n_audio_out, 1);
	CHECK(cph_layout_compare(&probe, l) != 0);
	cph_layout_clear(&probe);
	rig_close(&r);
}

static void test_process(void)
{
	struct rig r;
	uint32_t i;

	rig_open(&r, GAIN, 0);

	/* not active: dry pass-through, the plugin is not called */
	fill(&r, 0.25f, 128);
	cph_instance_process(r.inst, 128);
	CHECK_NEAR(r.out[0][0], 0.25f, 1e-7);
	CHECK_NEAR(r.out[1][127], 0.25f, 1e-7);
	CHECK_EQ_U(r.ext->process_calls(r.plugin), 0);

	CHECK(cph_instance_activate(r.inst) == 0);
	CHECK(cph_instance_activate(r.inst) == 0);	/* idempotent */
	fill(&r, 1.0f, 256);
	cph_instance_process(r.inst, 256);
	CHECK_NEAR(r.out[0][0], 1.0f, 1e-6);
	CHECK_NEAR(r.out[1][255], 1.0f, 1e-6);
	CHECK_EQ_U(r.ext->param_events(r.plugin), 0);	/* defaults are not sent */

	*ctl(&r, "Gain_dB") = -6.0206f;
	fill(&r, 1.0f, 256);
	cph_instance_process(r.inst, 256);
	CHECK_NEAR(r.out[0][10], 0.5f, 1e-4);
	CHECK_NEAR(r.out[1][200], 0.5f, 1e-4);
	CHECK_EQ_U(r.ext->param_events(r.plugin), 1);
	CHECK_NEAR(r.notify[0], 0.5f, 1e-4);		/* Peak, an output event */

	cph_instance_process(r.inst, 256);		/* unchanged: no new event */
	CHECK_EQ_U(r.ext->param_events(r.plugin), 1);

	*ctl(&r, "Gain_dB") = 100.0f;			/* clamped to the CLAP range */
	cph_instance_process(r.inst, 64);
	CHECK_NEAR(r.ext->value(r.plugin, 1), 12.0, 1e-9);

	*ctl(&r, "Mute") = 1.0f;
	fill(&r, 1.0f, 64);
	cph_instance_process(r.inst, 64);
	for (i = 0; i < 64; i++)
		CHECK(r.out[0][i] == 0.0f);

	cph_instance_deactivate(r.inst);
	cph_instance_deactivate(r.inst);		/* idempotent */
	fill(&r, 0.3f, 64);
	cph_instance_process(r.inst, 64);
	CHECK_NEAR(r.out[1][5], 0.3f, 1e-7);		/* back to pass-through */
	rig_close(&r);
}

static void test_chunks_and_aliasing(void)
{
	struct rig r;
	uint32_t i;

	rig_open(&r, GAIN, 64);
	CHECK(cph_instance_activate(r.inst) == 0);
	*ctl(&r, "Gain_dB") = -20.0f;
	for (i = 0; i < 200; i++)
		r.in[0][i] = r.in[1][i] = (float)i;
	cph_instance_process(r.inst, 200);
	CHECK(r.ext->max_frames_seen(r.plugin) <= 64);
	CHECK_EQ_U(r.ext->process_calls(r.plugin), 4);
	CHECK_EQ_U(r.ext->param_events(r.plugin), 1);	/* only in the first chunk */
	for (i = 0; i < 200; i++)
		CHECK_NEAR(r.out[0][i], (float)i * 0.1f, 1e-3);

	/* in-place: filter-graph may hand the same buffer in and out */
	cph_instance_connect(r.inst, (uint32_t)port_of(&r, "out_1"), r.in[0]);
	cph_instance_connect(r.inst, (uint32_t)port_of(&r, "out_2"), r.in[1]);
	for (i = 0; i < 100; i++)
		r.in[0][i] = r.in[1][i] = 1.0f;
	cph_instance_process(r.inst, 100);
	CHECK_NEAR(r.in[0][99], 0.1f, 1e-5);
	CHECK_NEAR(r.in[1][0], 0.1f, 1e-5);

	/* nothing connected at all still works */
	cph_instance_connect(r.inst, (uint32_t)port_of(&r, "in_1"), NULL);
	cph_instance_connect(r.inst, (uint32_t)port_of(&r, "out_1"), NULL);
	cph_instance_process(r.inst, 64);
	rig_close(&r);
}

static void test_restart_and_latency(void)
{
	struct rig r;
	struct cph_stats st;

	rig_open(&r, GAIN, 0);
	CHECK(cph_instance_activate(r.inst) == 0);
	CHECK(r.latency == 0.0f);
	fill(&r, 1.0f, 64);
	cph_instance_process(r.inst, 64);

	*ctl(&r, "Latency") = 100.0f;			/* the plugin asks for a restart */
	cph_instance_process(r.inst, 64);
	CHECK_EQ_I(n_pending, 1);
	CHECK_EQ_I(drain_main(), 1);			/* main asks audio to stop */
	fill(&r, 0.7f, 64);
	cph_instance_process(r.inst, 64);		/* audio stops; dry while restarting */
	CHECK_NEAR(r.out[0][3], 0.7f, 1e-7);
	CHECK_EQ_I(drain_main(), 1);			/* main deactivates + activates */
	CHECK(r.latency == 100.0f);
	CHECK_EQ_U(cph_instance_latency(r.inst), 100);
	*ctl(&r, "Gain_dB") = -6.0206f;
	fill(&r, 1.0f, 64);
	cph_instance_process(r.inst, 64);
	CHECK_NEAR(r.out[0][3], 0.5f, 1e-4);
	cph_instance_stats(r.inst, &st);
	CHECK_EQ_U(st.restarts, 1);
	CHECK_EQ_U(st.process_errors, 0);
	rig_close(&r);
}

struct thread_args {
	struct rig *r;
	int blocks;
};

static void *audio_thread(void *arg)
{
	struct thread_args *a = arg;
	int i;
	for (i = 0; i < a->blocks; i++)
		cph_instance_process(a->r->inst, 128);
	return NULL;
}

static void *other_thread(void *arg)
{
	struct rig *r = arg;
	r->ext->request_callback(r->plugin);
	return NULL;
}

static void test_threads_callbacks_log(void)
{
	struct rig r;
	struct thread_args a;
	pthread_t t;

	rig_open(&r, GAIN, 0);
	CHECK(cph_instance_activate(r.inst) == 0);
	a.r = &r;
	a.blocks = 20;
	fill(&r, 1.0f, 128);
	CHECK(pthread_create(&t, NULL, audio_thread, &a) == 0);
	pthread_join(t, NULL);
	CHECK_EQ_U(r.ext->process_calls(r.plugin), 20);

	/* on_main_thread requested from another thread runs on main */
	CHECK(pthread_create(&t, NULL, other_thread, &r) == 0);
	pthread_join(t, NULL);
	CHECK_EQ_I(drain_main(), 1);
	CHECK_EQ_U(r.ext->main_callbacks(r.plugin), 1);

	/* the plugin logs from the audio thread: queued, printed on main */
	r.ext->log_from_audio(r.plugin, true);
	*ctl(&r, "Gain_dB") = -1.0f;
	log_buf[0] = '\0';
	a.blocks = 1;
	CHECK(pthread_create(&t, NULL, audio_thread, &a) == 0);
	pthread_join(t, NULL);
	CHECK(strstr(log_buf, "gain changed") == NULL);
	CHECK_EQ_I(drain_main(), 1);
	CHECK(strstr(log_buf, "audio thread] gain changed on the audio thread") != NULL);

	/* stop_processing runs on the audio side during deactivate */
	cph_instance_deactivate(r.inst);
	rig_close(&r);
}

static void test_state_and_baseline(void)
{
	char path[] = "/tmp/clap-pw-state-XXXXXX";
	struct rig r;
	int fd = mkstemp(path);

	CHECK(fd >= 0);
	close(fd);

	rig_open(&r, GAIN, 0);
	CHECK(cph_instance_activate(r.inst) == 0);
	*ctl(&r, "Gain_dB") = -3.0f;
	*ctl(&r, "Mode") = 3.0f;
	cph_instance_process(r.inst, 32);
	cph_instance_deactivate(r.inst);
	CHECK(cph_instance_state_save(r.inst, path) == 0);
	rig_close(&r);

	/* A new instance restores the state; controls at their defaults do not
	 * override it, a control set in the config does. */
	rig_open(&r, GAIN, 0);
	CHECK(cph_instance_state_load(r.inst, path) == 0);
	CHECK_NEAR(r.ext->value(r.plugin, 1), -3.0, 1e-9);
	CHECK_NEAR(r.ext->value(r.plugin, 3), 3.0, 1e-9);
	*ctl(&r, "Mute") = 0.0f;			/* default */
	*ctl(&r, "Gain_dB_7") = 4.0f;			/* not default */
	CHECK(cph_instance_activate(r.inst) == 0);
	fill(&r, 1.0f, 32);
	cph_instance_process(r.inst, 32);
	CHECK_NEAR(r.ext->value(r.plugin, 1), -3.0, 1e-9);	/* kept from state */
	CHECK_NEAR(r.ext->value(r.plugin, 3), 3.0, 1e-9);
	CHECK_NEAR(r.ext->value(r.plugin, 7), 4.0, 1e-6);	/* sent */
	CHECK_NEAR(r.out[0][0], powf(10.0f, -3.0f / 20.0f), 1e-5);
	rig_close(&r);

	unlink(path);
}

static void test_text_and_set(void)
{
	struct rig r;
	double v;
	char txt[64];

	rig_open(&r, GAIN, 0);
	CHECK(cph_instance_param_parse(r.inst, (uint32_t)port_of(&r, "Mode"), "Hot", &v) == 0);
	CHECK(v == 2.0);
	CHECK(cph_instance_param_text(r.inst, (uint32_t)port_of(&r, "Mode"), 3, txt, sizeof(txt)) == 0);
	CHECK_STR(txt, "Broken");
	CHECK(cph_instance_param_set(r.inst, (uint32_t)port_of(&r, "Mode"), v) == 0);
	CHECK(r.ext->value(r.plugin, 3) == 2.0);
	CHECK(cph_instance_param_value(r.inst, (uint32_t)port_of(&r, "Mode"), &v) == 0 && v == 2.0);
	CHECK(cph_instance_param_set(r.inst, (uint32_t)port_of(&r, "Peak"), 1) == -EINVAL);
	CHECK(cph_instance_param_set(r.inst, (uint32_t)port_of(&r, "in_1"), 1) == -EINVAL);
	CHECK(cph_instance_activate(r.inst) == 0);
	CHECK(cph_instance_param_set(r.inst, (uint32_t)port_of(&r, "Mode"), 1) == -EBUSY);
	rig_close(&r);
}

static void test_zombie(void)
{
	struct rig r;

	rig_open(&r, MONO, 0);
	CHECK(cph_instance_activate(r.inst) == 0);
	r.ext->request_callback(r.plugin);
	CHECK_EQ_I(n_pending, 1);
	CHECK_EQ_U(r.ext->violations(r.plugin), 0);
	/* freed while a main-thread callback is still queued */
	cph_instance_free(r.inst);
	cph_bundle_unref(r.b);
	CHECK_EQ_I(drain_main(), 1);			/* releases the memory, no crash */
}

int main(void)
{
	RUN(test_sanitize);
	RUN(test_resolve);
	RUN(test_bundle);
	RUN(test_layout);
	RUN(test_process);
	RUN(test_chunks_and_aliasing);
	RUN(test_restart_and_latency);
	RUN(test_threads_callbacks_log);
	RUN(test_state_and_baseline);
	RUN(test_text_and_set);
	RUN(test_zombie);
	return check_summary();
}
