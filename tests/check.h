/* clap-pipewire: a minimal test harness */
/* SPDX-License-Identifier: MIT */
#ifndef CLAP_PIPEWIRE_CHECK_H
#define CLAP_PIPEWIRE_CHECK_H

#include <math.h>
#include <stdio.h>
#include <string.h>

static int check_failures;
static int check_count;

#define CHECK(cond) do {						\
	check_count++;							\
	if (!(cond)) {							\
		check_failures++;					\
		fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); \
	}								\
} while (0)

#define CHECK_EQ_U(a, b) do {						\
	unsigned long _a = (unsigned long)(a), _b = (unsigned long)(b);	\
	check_count++;							\
	if (_a != _b) {							\
		check_failures++;					\
		fprintf(stderr, "%s:%d: FAILED: %s == %s (%lu != %lu)\n",	\
				__FILE__, __LINE__, #a, #b, _a, _b);	\
	}								\
} while (0)

#define CHECK_EQ_I(a, b) do {						\
	long _a = (long)(a), _b = (long)(b);				\
	check_count++;							\
	if (_a != _b) {							\
		check_failures++;					\
		fprintf(stderr, "%s:%d: FAILED: %s == %s (%ld != %ld)\n",	\
				__FILE__, __LINE__, #a, #b, _a, _b);	\
	}								\
} while (0)

#define CHECK_NEAR(a, b, eps) do {					\
	double _a = (double)(a), _b = (double)(b);			\
	check_count++;							\
	if (!(fabs(_a - _b) <= (eps))) {				\
		check_failures++;					\
		fprintf(stderr, "%s:%d: FAILED: %s ~ %s (%g vs %g)\n",	\
				__FILE__, __LINE__, #a, #b, _a, _b);	\
	}								\
} while (0)

#define CHECK_STR(a, b) do {						\
	const char *_a = (a), *_b = (b);				\
	check_count++;							\
	if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) {		\
		check_failures++;					\
		fprintf(stderr, "%s:%d: FAILED: %s == %s (\"%s\" vs \"%s\")\n", \
				__FILE__, __LINE__, #a, #b, _a ? _a : "(null)", _b ? _b : "(null)"); \
	}								\
} while (0)

#define RUN(fn) do {							\
	int _before = check_failures;					\
	fn();								\
	fprintf(stderr, "%-34s %s\n", #fn, check_failures == _before ? "ok" : "FAIL"); \
} while (0)

static inline int check_summary(void)
{
	fprintf(stderr, "%d checks, %d failed\n", check_count, check_failures);
	return check_failures ? 1 : 0;
}

#endif
