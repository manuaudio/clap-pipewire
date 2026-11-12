/* clap-pipewire: drive the built SPA plugin the way filter-graph does */
/* SPDX-License-Identifier: MIT */

#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/utils/dict.h>
#include <spa/utils/string.h>

#include "fga/audio-plugin.h"
#include "check.h"

static char log_buf[16384];

/* the plugin asks the process which PipeWire it is running in */
static const char *fake_version = "1.6.8";
__attribute__((visibility("default"))) const char *pw_get_library_version(void);
const char *pw_get_library_version(void)
{
	return fake_version;
}

static void vlog(enum spa_log_level level, const char *fmt, va_list ap)
{
	size_t n = strlen(log_buf);
	vsnprintf(log_buf + n, sizeof(log_buf) - n, fmt, ap);
	n = strlen(log_buf);
	if (n + 1 < sizeof(log_buf)) {
		log_buf[n] = '\n';
		log_buf[n + 1] = '\0';
	}
	if (getenv("TEST_VERBOSE")) {
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	}
}

static void t_log(void *object, enum spa_log_level level, const char *file, int line,
		const char *func, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vlog(level, fmt, ap);
	va_end(ap);
}

static void t_logv(void *object, enum spa_log_level level, const char *file, int line,
		const char *func, const char *fmt, va_list args)
{
	vlog(level, fmt, args);
}

static void t_logt(void *object, enum spa_log_level level, const struct spa_log_topic *topic,
		const char *file, int line, const char *func, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vlog(level, fmt, ap);
	va_end(ap);
}

static void t_logtv(void *object, enum spa_log_level level, const struct spa_log_topic *topic,
		const char *file, int line, const char *func, const char *fmt, va_list args)
{
	vlog(level, fmt, args);
}

static const struct spa_log_methods log_methods = {
	SPA_VERSION_LOG_METHODS, .log = t_log, .logv = t_logv, .logt = t_logt, .logtv = t_logtv,
};
static struct spa_log test_log;

/* main loop: queue; data loop: run in place */
struct queued {
	spa_invoke_func_t func;
	void *user_data;
};
static struct queued main_q[32];
static int n_main_q;

static int main_invoke(void *object, spa_invoke_func_t func, uint32_t seq, const void *data,
		size_t size, bool block, void *user_data)
{
	if (n_main_q >= 32)
		return -ENOSPC;
	main_q[n_main_q].func = func;
	main_q[n_main_q].user_data = user_data;
	n_main_q++;
	return 0;
}

static int data_calls;
static int data_invoke(void *object, spa_invoke_func_t func, uint32_t seq, const void *data,
		size_t size, bool block, void *user_data)
{
	data_calls++;
	return func ? func(NULL, false, seq, data, size, user_data) : 0;
}

static const struct spa_loop_methods main_methods = { SPA_VERSION_LOOP_METHODS, .invoke = main_invoke };
static const struct spa_loop_methods data_methods = { SPA_VERSION_LOOP_METHODS, .invoke = data_invoke };
static struct spa_loop main_loop, data_loop;

static int drain_main(void)
{
	int i, n = n_main_q;
	n_main_q = 0;
	for (i = 0; i < n; i++)
		main_q[i].func(&main_loop, false, 0, NULL, 0, main_q[i].user_data);
	return n;
}

struct plugin_handle {
	void *dl;
	const struct spa_handle_factory *factory;
	struct spa_handle *handle;
	struct spa_fga_plugin *plugin;
};

static int open_plugin(struct plugin_handle *ph, const char *path)
{
	int (*enum_func)(const struct spa_handle_factory **, uint32_t *);
	struct spa_support support[3];
	uint32_t idx = 0;
	void *iface;
	int res;

	memset(ph, 0, sizeof(*ph));
	ph->dl = dlopen(SPA_PLUGIN_PATH, RTLD_NOW);
	CHECK(ph->dl != NULL);
	enum_func = dlsym(ph->dl, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	CHECK(enum_func != NULL);
	CHECK(enum_func(&ph->factory, &idx) == 1);
	CHECK_STR(ph->factory->name, "filter.graph.plugin.clap");

	support[0] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Log, &test_log);
	support[1] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Loop, &main_loop);
	support[2] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_DataLoop, &data_loop);

	struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT("filter.graph.path", path),
		SPA_DICT_ITEM_INIT("library.filter-path", "false"),
	};
	struct spa_dict info = SPA_DICT_INIT_ARRAY(items);

	ph->handle = calloc(1, spa_handle_factory_get_size(ph->factory, &info));
	res = spa_handle_factory_init(ph->factory, ph->handle, &info, support, 3);
	if (res < 0) {
		free(ph->handle);
		ph->handle = NULL;
		return res;
	}
	CHECK(spa_handle_get_interface(ph->handle, SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioPlugin, &iface) == 0);
	ph->plugin = iface;
	return 0;
}

static void close_plugin(struct plugin_handle *ph)
{
	if (ph->handle) {
		spa_handle_clear(ph->handle);
		free(ph->handle);
	}
	if (ph->dl)
		dlclose(ph->dl);
}

static int find_port(const struct spa_fga_descriptor *d, const char *name)
{
	uint32_t i;
	for (i = 0; i < d->n_ports; i++)
		if (spa_streq(d->ports[i].name, name))
			return (int)i;
	return -1;
}

/* what filter-graph does: one float per control, buffers per audio port */
struct graph {
	const struct spa_fga_descriptor *d;
	void *inst;
	float ctl[32];
	float in[2][512], out[2][512];
};

static void graph_connect(struct graph *g)
{
	uint32_t i, ai = 0, ao = 0;
	for (i = 0; i < g->d->n_ports; i++) {
		struct spa_fga_port *p = &g->d->ports[i];
		if (SPA_FGA_IS_PORT_AUDIO(p->flags)) {
			if (SPA_FGA_IS_PORT_INPUT(p->flags))
				g->d->connect_port(g->inst, i, g->in[ai++]);
			else
				g->d->connect_port(g->inst, i, g->out[ao++]);
		} else {
			g->d->connect_port(g->inst, i, &g->ctl[i]);
		}
	}
}

static void test_missing(void)
{
	struct plugin_handle ph;
	log_buf[0] = '\0';
	CHECK(open_plugin(&ph, "no-such-bundle.clap") < 0);
	CHECK(strstr(log_buf, "can't find bundle 'no-such-bundle.clap'") != NULL);
	close_plugin(&ph);
	CHECK(open_plugin(&ph, "") < 0);
	close_plugin(&ph);
}

static void test_abi_guard(void)
{
	struct plugin_handle ph;

	fake_version = "1.7.2";
	log_buf[0] = '\0';
	CHECK(open_plugin(&ph, TEST_PLUGIN_PATH) == -ENOTSUP);
	CHECK(strstr(log_buf, "built for the PipeWire 1.6.x filter-graph plugin ABI") != NULL);
	close_plugin(&ph);

	setenv("CLAP_PW_IGNORE_ABI", "true", 1);
	CHECK(open_plugin(&ph, TEST_PLUGIN_PATH) == 0);
	close_plugin(&ph);
	unsetenv("CLAP_PW_IGNORE_ABI");

	fake_version = "1.6.12";
	CHECK(open_plugin(&ph, TEST_PLUGIN_PATH) == 0);
	close_plugin(&ph);
	fake_version = "1.6.8";
}

static void test_descriptor(void)
{
	struct plugin_handle ph;
	const struct spa_fga_descriptor *d;
	int p;

	CHECK(open_plugin(&ph, TEST_PLUGIN_PATH) == 0);
	log_buf[0] = '\0';
	CHECK(spa_fga_plugin_make_desc(ph.plugin, "") == NULL);	/* two plugins inside */
	CHECK(strstr(log_buf, "set label to a plugin id") != NULL);
	CHECK(strstr(log_buf, "org.clap-pipewire.test.mono") != NULL);
	CHECK(spa_fga_plugin_make_desc(ph.plugin, "nope") == NULL);

	d = spa_fga_plugin_make_desc(ph.plugin, "org.clap-pipewire.test.gain");
	CHECK(d != NULL);
	CHECK_STR(d->name, "org.clap-pipewire.test.gain");
	CHECK_EQ_U(d->n_ports, 11);
	CHECK(d->ports[0].flags == (SPA_FGA_PORT_INPUT | SPA_FGA_PORT_AUDIO));
	CHECK(d->ports[3].flags == (SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_AUDIO));
	p = find_port(d, "Gain_dB");
	CHECK(p == 4);
	CHECK(d->ports[p].flags == (SPA_FGA_PORT_INPUT | SPA_FGA_PORT_CONTROL));
	CHECK(d->ports[p].min == -60.0f && d->ports[p].max == 12.0f && d->ports[p].def == 0.0f);
	CHECK(d->ports[find_port(d, "Mute")].hint & SPA_FGA_HINT_BOOLEAN);
	CHECK(d->ports[find_port(d, "Mode")].hint & SPA_FGA_HINT_INTEGER);
	CHECK(d->ports[find_port(d, "Peak")].flags == (SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL));
	p = find_port(d, "latency");
	CHECK(p == 10);
	CHECK(d->ports[p].hint & SPA_FGA_HINT_LATENCY);
	CHECK(d->ports[p].flags == (SPA_FGA_PORT_OUTPUT | SPA_FGA_PORT_CONTROL));
	spa_fga_descriptor_free(d);

	d = spa_fga_plugin_make_desc(ph.plugin, "Test Mono Gain");	/* by name */
	CHECK(d != NULL && d->n_ports == 2 + 5 + 1 + 1);
	if (d)
		spa_fga_descriptor_free(d);
	close_plugin(&ph);
}

static void run_block(struct graph *g, float v, uint32_t n)
{
	uint32_t i;
	for (i = 0; i < n; i++) {
		g->in[0][i] = g->in[1][i] = v;
		g->out[0][i] = g->out[1][i] = -99.0f;
	}
	g->d->run(g->inst, n);
}

static void test_lifecycle(void)
{
	char state[] = "/tmp/clap-pw-spa-state-XXXXXX";
	char config[512];
	struct plugin_handle ph;
	struct graph g;
	int fd, gain, lat, mode;
	uint32_t i;

	CHECK((fd = mkstemp(state)) >= 0);
	close(fd);
	unlink(state);	/* no state yet: must not be an error */

	CHECK(open_plugin(&ph, TEST_PLUGIN_PATH) == 0);
	memset(&g, 0, sizeof(g));
	g.d = spa_fga_plugin_make_desc(ph.plugin, "org.clap-pipewire.test.gain");
	CHECK(g.d != NULL);
	gain = find_port(g.d, "Gain_dB");
	lat = find_port(g.d, "latency");
	mode = find_port(g.d, "Mode");
	for (i = 0; i < g.d->n_ports; i++)
		g.ctl[i] = g.d->ports[i].def;

	snprintf(config, sizeof(config),
			"{ state = \"%s\" save-state = true max-block = 128 "
			"params = { \"Mode\" = \"Hot\" \"Gain (dB)\" = -3.5 \"Nope\" = 1 } }", state);
	log_buf[0] = '\0';
	g.inst = g.d->instantiate(ph.plugin, g.d, 48000, 0, config);
	CHECK(g.inst != NULL);
	CHECK(strstr(log_buf, "no state yet") != NULL);
	CHECK(strstr(log_buf, "no parameter 'Nope'") != NULL);
	graph_connect(&g);
	g.d->activate(g.inst);
	CHECK(g.ctl[lat] == 0.0f);

	/* config params applied through flush; controls at defaults not sent */
	run_block(&g, 1.0f, 300);	/* > max-block: split */
	CHECK_NEAR(g.out[0][0], powf(10.0f, -3.5f / 20.0f), 1e-5);
	CHECK_NEAR(g.out[1][299], powf(10.0f, -3.5f / 20.0f), 1e-5);

	/* a live control change, as pw-cli set-param Props would do */
	g.ctl[gain] = -6.0206f;
	run_block(&g, 1.0f, 64);
	CHECK_NEAR(g.out[0][10], 0.5f, 1e-4);
	CHECK_NEAR(g.ctl[find_port(g.d, "Peak")], 0.5f, 1e-4);

	/* filter-graph's reset: deactivate + activate */
	g.d->deactivate(g.inst);
	CHECK(data_calls >= 1);		/* stop_processing went through the data loop */
	g.d->activate(g.inst);
	run_block(&g, 1.0f, 64);
	CHECK_NEAR(g.out[0][10], 0.5f, 1e-4);

	g.d->deactivate(g.inst);
	g.d->cleanup(g.inst);		/* saves state */
	CHECK(access(state, R_OK) == 0);
	drain_main();

	/* a new instance (filter-graph re-instantiates on every activation)
	 * restores it; the control mirrors still hold the live values */
	g.ctl[gain] = g.d->ports[gain].def;	/* as if from a fresh graph */
	g.ctl[mode] = g.d->ports[mode].def;
	snprintf(config, sizeof(config), "{ state = \"%s\" }", state);
	log_buf[0] = '\0';
	g.inst = g.d->instantiate(ph.plugin, g.d, 44100, 0, config);
	CHECK(g.inst != NULL);
	CHECK(strstr(log_buf, "state loaded") != NULL);
	graph_connect(&g);
	g.d->activate(g.inst);
	run_block(&g, 1.0f, 64);
	CHECK_NEAR(g.out[0][10], 0.5f, 1e-4);	/* -6 dB came back from the state */
	g.d->deactivate(g.inst);
	g.d->cleanup(g.inst);
	drain_main();

	CHECK(g.d->instantiate(ph.plugin, g.d, 48000, 0, "{ max-block = 3 }") == NULL);
	CHECK(g.d->instantiate(ph.plugin, g.d, 48000, 0, "[ 1 2 ]") == NULL);

	spa_fga_descriptor_free(g.d);
	close_plugin(&ph);
	unlink(state);
}

int main(void)
{
	test_log.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Log, SPA_VERSION_LOG, &log_methods, NULL);
	test_log.level = SPA_LOG_LEVEL_DEBUG;
	main_loop.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Loop, SPA_VERSION_LOOP, &main_methods, NULL);
	data_loop.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_DataLoop, SPA_VERSION_LOOP, &data_methods, NULL);

	RUN(test_abi_guard);
	RUN(test_missing);
	RUN(test_descriptor);
	RUN(test_lifecycle);
	return check_summary();
}
