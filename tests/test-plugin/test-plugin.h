/* clap-pipewire: the test plugin's private extension */
/* SPDX-License-Identifier: MIT */
#ifndef CLAP_PIPEWIRE_TEST_PLUGIN_H
#define CLAP_PIPEWIRE_TEST_PLUGIN_H

#include <clap/clap.h>

#define TEST_EXT_ID "org.clap-pipewire.test/1"

struct test_plugin_ext {
	uint32_t (*violations)(const clap_plugin_t *p);
	uint32_t (*param_events)(const clap_plugin_t *p);
	uint32_t (*main_callbacks)(const clap_plugin_t *p);
	uint32_t (*process_calls)(const clap_plugin_t *p);
	uint32_t (*max_frames_seen)(const clap_plugin_t *p);
	double (*value)(const clap_plugin_t *p, clap_id id);
	void (*request_callback)(const clap_plugin_t *p);
	void (*log_from_audio)(const clap_plugin_t *p, bool on);
};

#endif
