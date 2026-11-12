/* clap-pipewire: list CLAP plugins the way filter-graph will see them */
/* SPDX-FileCopyrightText: Copyright © 2026 manuaudio */
/* SPDX-License-Identifier: MIT */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "clap-host.h"

static FILE *out;
static int verbose;
static int check_mode;

static void log_cb(void *data, enum cph_log_level level, const char *msg)
{
	(void)data;
	if (level <= CPH_LOG_WARN || verbose)
		fprintf(stderr, "%s\n", msg);
}

static const struct cph_log logger = { log_cb, NULL };

/* Plugins print to stdout and some crash: run each probe in a child with
 * stdout pointed away, and write our own output to a saved descriptor. */
static int in_child(int (*fn)(const char *, const char *), const char *a, const char *b)
{
	pid_t pid;
	int status, fd;

	fflush(out);
	pid = fork();
	if (pid < 0)
		return -errno;
	if (pid == 0) {
		alarm(20);
		if ((fd = open("/dev/null", O_WRONLY)) >= 0) {
			dup2(fd, STDOUT_FILENO);
			close(fd);
		}
		fd = fn(a, b);
		fflush(out);
		_exit(fd < 0 ? 1 : 0);
	}
	if (waitpid(pid, &status, 0) < 0)
		return -errno;
	if (WIFSIGNALED(status)) {
		fprintf(out, "  !! probe died: %s\n", strsignal(WTERMSIG(status)));
		return -EIO;
	}
	return WEXITSTATUS(status) == 0 ? 0 : -EIO;
}

static int list_bundle(const char *path, const char *unused)
{
	struct cph_bundle *b;
	uint32_t i, n;
	(void)unused;

	if ((b = cph_bundle_open(path, &logger)) == NULL)
		return -EIO;
	n = cph_bundle_n_plugins(b);
	for (i = 0; i < n; i++) {
		const clap_plugin_descriptor_t *d = cph_bundle_descriptor(b, i);
		const char *const *f;
		fprintf(out, "  %-44s %s", d->id, d->name);
		if (d->features) {
			fprintf(out, "  [");
			for (f = d->features; *f; f++)
				fprintf(out, "%s%s", f == d->features ? "" : " ", *f);
			fprintf(out, "]");
		}
		fprintf(out, "\n");
	}
	cph_bundle_unref(b);
	return 0;
}

static const char *kind_name(enum cph_port_kind k)
{
	switch (k) {
	case CPH_PORT_AUDIO_IN: return "audio in";
	case CPH_PORT_AUDIO_OUT: return "audio out";
	case CPH_PORT_PARAM_IN: return "control";
	case CPH_PORT_PARAM_OUT: return "notify";
	case CPH_PORT_LATENCY: return "latency";
	}
	return "?";
}

/* -c: activate, run 2 s of a 1 kHz tone at -12 dBFS on a separate audio
 * thread, and check the output is finite and not silent. */
#define CHECK_BLOCK 512
struct check_run {
	struct cph_instance *inst;
	float **in, **out;
	uint32_t n_in, n_out;
	double *sum;
	bool finite;
};

static void *check_thread(void *arg)
{
	struct check_run *r = arg;
	uint32_t b, c, i;
	long n = 0;

	for (b = 0; b < 2 * 48000 / CHECK_BLOCK; b++) {
		for (i = 0; i < CHECK_BLOCK; i++, n++) {
			float v = 0.25f * sinf(2.0f * (float)M_PI * 1000.0f * (float)n / 48000.0f);
			for (c = 0; c < r->n_in; c++)
				r->in[c][i] = v;
		}
		cph_instance_process(r->inst, CHECK_BLOCK);
		for (c = 0; c < r->n_out; c++)
			for (i = 0; i < CHECK_BLOCK; i++) {
				if (!isfinite(r->out[c][i]))
					r->finite = false;
				else if (b >= 48000 / CHECK_BLOCK)	/* the second second */
					r->sum[c] += (double)r->out[c][i] * r->out[c][i];
			}
	}
	return NULL;
}

static int check_plugin(struct cph_instance *inst)
{
	const struct cph_layout *l = cph_instance_layout(inst);
	struct check_run r = { .inst = inst, .n_in = l->n_audio_in, .n_out = l->n_audio_out, .finite = true };
	float *ctl = calloc(l->n_ports + 1, sizeof(float));
	struct cph_stats st;
	pthread_t t;
	uint32_t i, c;
	bool silent = true;
	int res = 0;

	r.in = calloc(r.n_in + 1, sizeof(float *));
	r.out = calloc(r.n_out + 1, sizeof(float *));
	r.sum = calloc(r.n_out + 1, sizeof(double));
	for (c = 0; c < r.n_in; c++)
		r.in[c] = calloc(CHECK_BLOCK, sizeof(float));
	for (c = 0; c < r.n_out; c++)
		r.out[c] = calloc(CHECK_BLOCK, sizeof(float));
	for (i = 0; i < l->n_ports; i++) {
		const struct cph_port *p = &l->ports[i];
		switch (p->kind) {
		case CPH_PORT_AUDIO_IN: cph_instance_connect(inst, i, r.in[p->slot]); break;
		case CPH_PORT_AUDIO_OUT: cph_instance_connect(inst, i, r.out[p->slot]); break;
		case CPH_PORT_PARAM_IN: ctl[i] = (float)p->def; /* fall through */
		default: cph_instance_connect(inst, i, &ctl[i]); break;
		}
	}
	if (cph_instance_activate(inst) < 0) {
		fprintf(out, "  check: FAIL (activate)\n");
		res = -EIO;
		goto done;
	}
	pthread_create(&t, NULL, check_thread, &r);
	pthread_join(t, NULL);
	cph_instance_main_callback(inst);
	cph_instance_deactivate(inst);
	cph_instance_stats(inst, &st);

	fprintf(out, "  check: 2 s of 1 kHz at -12 dBFS, %u-frame blocks; output RMS dBFS:", CHECK_BLOCK);
	for (c = 0; c < r.n_out; c++) {
		double rms = sqrt(r.sum[c] / 48000.0);
		if (rms > 1e-7)
			silent = false;
		fprintf(out, " %.1f", rms > 0 ? 20.0 * log10(rms) : -200.0);
	}
	fprintf(out, "\n  check: latency %u, process errors %u -> %s\n", cph_instance_latency(inst),
			st.process_errors, !r.finite || st.process_errors ? "FAIL" :
			silent && r.n_out ? "WARN (silent with default parameters)" : "PASS");
	if (!r.finite || st.process_errors)
		res = -EIO;
done:
	for (c = 0; c < r.n_in; c++)
		free(r.in[c]);
	for (c = 0; c < r.n_out; c++)
		free(r.out[c]);
	free(r.in);
	free(r.out);
	free(r.sum);
	free(ctl);
	return res;
}

static int n_conv;
static char **conv;

/* NAME=TEXT: print the float filter-graph wants for a value the plugin shows */
static int convert_values(struct cph_instance *inst)
{
	const struct cph_layout *l = cph_instance_layout(inst);
	int i, res = 0;

	for (i = 0; i < n_conv; i++) {
		char name[CPH_NAME_MAX], txt[128] = "";
		const char *eq = strchr(conv[i], '=');
		double v;
		int port;

		if (eq == NULL || (size_t)(eq - conv[i]) >= sizeof(name)) {
			fprintf(stderr, "expected NAME=TEXT, got '%s'\n", conv[i]);
			res = -EINVAL;
			continue;
		}
		snprintf(name, sizeof(name), "%.*s", (int)(eq - conv[i]), conv[i]);
		if ((port = cph_layout_find(l, name)) < 0) {
			fprintf(stderr, "no parameter '%s'\n", name);
			res = -ENOENT;
			continue;
		}
		if (cph_instance_param_parse(inst, (uint32_t)port, eq + 1, &v) < 0) {
			fprintf(stderr, "%s: the plugin can't parse '%s'\n", name, eq + 1);
			res = -EINVAL;
			continue;
		}
		cph_instance_param_text(inst, (uint32_t)port, v, txt, sizeof(txt));
		fprintf(out, "\"%s\" = %.9g    # %s\n", l->ports[port].name, v, txt);
	}
	return res;
}

static int show_plugin(const char *path, const char *id)
{
	struct cph_instance_opts opts = { .sample_rate = 48000, .log = &logger };
	struct cph_bundle *b;
	struct cph_instance *inst;
	const struct cph_layout *l;
	const clap_plugin_descriptor_t *d;
	uint32_t i;

	if ((b = cph_bundle_open(path, &logger)) == NULL)
		return -EIO;
	if ((inst = cph_instance_new(b, id, &opts, NULL)) == NULL) {
		cph_bundle_unref(b);
		return -EIO;
	}
	d = cph_instance_descriptor(inst);
	l = cph_instance_layout(inst);
	if (n_conv > 0) {
		int res = convert_values(inst);
		cph_instance_free(inst);
		cph_bundle_unref(b);
		return res;
	}
	fprintf(out, "%s\n  id:      %s\n  vendor:  %s\n  version: %s\n  bundle:  %s\n",
			d->name, d->id, d->vendor ? d->vendor : "", d->version ? d->version : "",
			cph_bundle_path(b));
	fprintf(out, "  audio:   %u in, %u out (CLAP ports: %u in, %u out)\n",
			l->n_audio_in, l->n_audio_out, l->n_clap_in, l->n_clap_out);
	fprintf(out, "  params:  %u controls, %u read-only\n\n", l->n_params_in, l->n_params_out);
	fprintf(out, "  %-4s %-10s %-32s %12s %12s %12s  %s\n", "#", "kind", "filter-graph name",
			"default", "min", "max", "CLAP name / now");

	for (i = 0; i < l->n_ports; i++) {
		const struct cph_port *p = &l->ports[i];
		if (p->kind == CPH_PORT_PARAM_IN || p->kind == CPH_PORT_PARAM_OUT) {
			char txt[128] = "";
			double v = p->def;
			cph_instance_param_value(inst, i, &v);
			cph_instance_param_text(inst, i, v, txt, sizeof(txt));
			fprintf(out, "  %-4u %-10s %-32s %12g %12g %12g  %s%s%s%s%s\n", i, kind_name(p->kind),
					p->name, p->def, p->min, p->max, p->module[0] ? p->module : "",
					p->module[0] ? "/" : "", p->label, txt[0] ? " = " : "", txt);
		} else {
			fprintf(out, "  %-4u %-10s %s\n", i, kind_name(p->kind), p->name);
		}
	}

	fprintf(out, "\n  filter.graph node:\n"
			"    { type = clap name = fx plugin = \"%s\" label = \"%s\" }\n"
			"    inputs = [", cph_bundle_path(b), d->id);
	for (i = 0; i < l->n_ports; i++)
		if (l->ports[i].kind == CPH_PORT_AUDIO_IN)
			fprintf(out, " \"fx:%s\"", l->ports[i].name);
	fprintf(out, " ]  outputs = [");
	for (i = 0; i < l->n_ports; i++)
		if (l->ports[i].kind == CPH_PORT_AUDIO_OUT)
			fprintf(out, " \"fx:%s\"", l->ports[i].name);
	fprintf(out, " ]\n");

	if (check_mode) {
		int res;
		fprintf(out, "\n");
		res = check_plugin(inst);
		cph_instance_free(inst);
		cph_bundle_unref(b);
		return res;
	}
	cph_instance_free(inst);
	cph_bundle_unref(b);
	return 0;
}

static bool is_clap(const char *name)
{
	size_t n = strlen(name);
	return n > 5 && strcmp(name + n - 5, ".clap") == 0;
}

static void scan_dir(const char *dir, int depth)
{
	struct dirent **ents;
	int i, n;

	if ((n = scandir(dir, &ents, NULL, alphasort)) < 0)
		return;
	for (i = 0; i < n; i++) {
		char path[PATH_MAX];
		struct stat st;
		const char *name = ents[i]->d_name;

		if (name[0] == '.')
			goto next;
		snprintf(path, sizeof(path), "%s/%s", dir, name);
		if (stat(path, &st) < 0)
			goto next;
		if (S_ISDIR(st.st_mode)) {
			if (depth < 2)
				scan_dir(path, depth + 1);
		} else if (S_ISREG(st.st_mode) && is_clap(name)) {
			fprintf(out, "%s\n", path);
			in_child(list_bundle, path, NULL);
		}
next:
		free(ents[i]);
	}
	free(ents);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-v]                 list every bundle in the CLAP search path\n"
		"       %s [-v] BUNDLE          list the plugins in one bundle\n"
		"       %s [-v] [-c] BUNDLE ID  show ports and parameters as filter-graph names;\n"
		"                                 -c also runs a short processing check\n"
		"       %s BUNDLE ID NAME=TEXT...  convert a displayed value (\"Frequency=500 Hz\")\n"
		"                                 to the number a control wants\n"
		"BUNDLE is a path, or a name looked up in the search path.\n",
		prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
	char sp[4096], path[PATH_MAX], here[PATH_MAX];
	int fd, a = 1, res;

	if ((fd = dup(STDOUT_FILENO)) < 0 || (out = fdopen(fd, "w")) == NULL)
		return 1;
	setvbuf(out, NULL, _IOLBF, 0);

	while (a < argc && argv[a][0] == '-') {
		if (strcmp(argv[a], "-v") == 0)
			verbose = 1;
		else if (strcmp(argv[a], "-c") == 0)
			check_mode = 1;
		else {
			usage(argv[0]);
			return strcmp(argv[a], "-h") == 0 || strcmp(argv[a], "--help") == 0 ? 0 : 2;
		}
		a++;
	}

	if (a >= argc) {
		char *s, *save = NULL;
		cph_search_path(sp, sizeof(sp));
		fprintf(out, "# CLAP search path: %s\n", sp);
		for (s = strtok_r(sp, ":", &save); s; s = strtok_r(NULL, ":", &save))
			scan_dir(s, 0);
		return 0;
	}
	/* a path relative to here (./x.clap, build/x.clap) before the search path */
	if (strchr(argv[a], '/') && argv[a][0] != '/' && realpath(argv[a], here) != NULL)
		res = cph_resolve(here, path, sizeof(path));
	else
		res = cph_resolve(argv[a], path, sizeof(path));
	if (res < 0) {
		fprintf(stderr, "%s: %s\n", argv[a], strerror(-res));
		return 1;
	}
	if (a + 1 >= argc) {
		fprintf(out, "%s\n", path);
		return in_child(list_bundle, path, NULL) < 0 ? 1 : 0;
	}
	n_conv = argc - (a + 2);
	conv = argv + a + 2;
	return in_child(show_plugin, path, argv[a + 1]) < 0 ? 1 : 0;
}
