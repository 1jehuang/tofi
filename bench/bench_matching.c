#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "matching.h"
#include "string_vec.h"
#include "xmalloc.h"

static uint64_t monotonic_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void usage(FILE *out)
{
	fprintf(out,
			"Usage: tofi-bench-matching --pattern PATTERN [options]\n"
			"\n"
			"Options:\n"
			"  --dataset FILE        Newline-delimited candidate strings\n"
			"  --algorithm ALG       normal|prefix|fuzzy|typo (default: typo)\n"
			"  --mode MODE           match|filter (default: match)\n"
			"  --runs N              Timed runs (default: 50)\n"
			"  --warmup N            Warmup runs (default: 10)\n");
}

struct dataset {
	char *buf;
	char **lines;
	size_t count;
};

static struct dataset dataset_from_file(const char *path)
{
	struct dataset ds = {0};

	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		fprintf(stderr, "Failed to open dataset \"%s\": %s\n", path, strerror(errno));
		exit(2);
	}

	if (fseek(f, 0, SEEK_END) != 0) {
		fprintf(stderr, "Failed to seek dataset \"%s\": %s\n", path, strerror(errno));
		exit(2);
	}
	long size = ftell(f);
	if (size < 0) {
		fprintf(stderr, "Failed to tell dataset \"%s\": %s\n", path, strerror(errno));
		exit(2);
	}
	rewind(f);

	ds.buf = xmalloc((size_t)size + 1);
	size_t nread = fread(ds.buf, 1, (size_t)size, f);
	if (nread != (size_t)size && ferror(f)) {
		fprintf(stderr, "Failed to read dataset \"%s\": %s\n", path, strerror(errno));
		exit(2);
	}
	ds.buf[nread] = '\0';
	fclose(f);

	size_t cap = 1024;
	ds.lines = xcalloc(cap, sizeof(*ds.lines));

	char *p = ds.buf;
	while (*p != '\0') {
		char *line = p;
		while (*p != '\0' && *p != '\n') {
			p++;
		}
		if (*p == '\n') {
			*p = '\0';
			p++;
		}
		if (*line == '\0') {
			continue;
		}
		if (ds.count == cap) {
			cap *= 2;
			ds.lines = xrealloc(ds.lines, cap * sizeof(*ds.lines));
		}
		ds.lines[ds.count++] = line;
	}

	return ds;
}

static void dataset_destroy(struct dataset *ds)
{
	free(ds->lines);
	free(ds->buf);
	ds->lines = NULL;
	ds->buf = NULL;
	ds->count = 0;
}

static enum matching_algorithm parse_algorithm(const char *s)
{
	if (strcmp(s, "normal") == 0) {
		return MATCHING_ALGORITHM_NORMAL;
	}
	if (strcmp(s, "prefix") == 0) {
		return MATCHING_ALGORITHM_PREFIX;
	}
	if (strcmp(s, "fuzzy") == 0) {
		return MATCHING_ALGORITHM_FUZZY;
	}
	if (strcmp(s, "typo") == 0) {
		return MATCHING_ALGORITHM_TYPO;
	}

	fprintf(stderr, "Unknown algorithm \"%s\".\n", s);
	exit(2);
}

int main(int argc, char **argv)
{
	const char *dataset_path = NULL;
	const char *pattern = NULL;
	enum matching_algorithm alg = MATCHING_ALGORITHM_TYPO;
	bool filter_mode = false;
	uint32_t runs = 50;
	uint32_t warmup = 10;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--dataset") == 0 && i + 1 < argc) {
			dataset_path = argv[++i];
		} else if (strcmp(argv[i], "--pattern") == 0 && i + 1 < argc) {
			pattern = argv[++i];
		} else if (strcmp(argv[i], "--algorithm") == 0 && i + 1 < argc) {
			alg = parse_algorithm(argv[++i]);
		} else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
			const char *mode = argv[++i];
			if (strcmp(mode, "match") == 0) {
				filter_mode = false;
			} else if (strcmp(mode, "filter") == 0) {
				filter_mode = true;
			} else {
				fprintf(stderr, "Unknown mode \"%s\".\n", mode);
				return 2;
			}
		} else if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc) {
			runs = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
			warmup = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			usage(stdout);
			return 0;
		} else {
			fprintf(stderr, "Unknown argument \"%s\".\n", argv[i]);
			usage(stderr);
			return 2;
		}
	}

	if (pattern == NULL) {
		usage(stderr);
		return 2;
	}
	if (dataset_path == NULL) {
		fprintf(stderr, "--dataset is required (use a newline-delimited file).\n");
		return 2;
	}

	struct dataset ds = dataset_from_file(dataset_path);
	if (ds.count == 0) {
		fprintf(stderr, "Dataset is empty.\n");
		return 2;
	}

	struct match_query query = match_query_create(pattern);
	struct string_ref_vec vec = {0};
	if (filter_mode) {
		vec = string_ref_vec_create();
		for (size_t i = 0; i < ds.count; i++) {
			string_ref_vec_add(&vec, ds.lines[i]);
		}
	}

	/* Warmup */
	for (uint32_t r = 0; r < warmup; r++) {
		if (filter_mode) {
			struct string_ref_vec res = string_ref_vec_filter(&vec, pattern, alg);
			string_ref_vec_destroy(&res);
		} else {
			for (size_t i = 0; i < ds.count; i++) {
				(void)match_query_words(alg, &query, ds.lines[i]);
			}
		}
	}

	uint64_t total_ns = 0;
	size_t last_matches = 0;
	int64_t last_score_sum = 0;

	for (uint32_t r = 0; r < runs; r++) {
		uint64_t start = monotonic_ns();
		size_t matches = 0;
		int64_t score_sum = 0;

		if (filter_mode) {
			struct string_ref_vec res = string_ref_vec_filter(&vec, pattern, alg);
			matches = res.count;
			for (size_t i = 0; i < res.count; i++) {
				score_sum += res.buf[i].search_score;
			}
			string_ref_vec_destroy(&res);
		} else {
			for (size_t i = 0; i < ds.count; i++) {
				int32_t score = match_query_words(alg, &query, ds.lines[i]);
				if (score != INT32_MIN) {
					matches++;
					score_sum += score;
				}
			}
		}

		uint64_t end = monotonic_ns();
		total_ns += end - start;

		last_matches = matches;
		last_score_sum = score_sum;
	}

	double avg_ns = (double)total_ns / (double)runs;
	double avg_ns_per_item = avg_ns / (double)ds.count;

	printf("dataset_items=%zu\n", ds.count);
	printf("pattern=%s\n", pattern);
	printf("algorithm=%s\n", (alg == MATCHING_ALGORITHM_NORMAL) ? "normal" :
			(alg == MATCHING_ALGORITHM_PREFIX) ? "prefix" :
			(alg == MATCHING_ALGORITHM_FUZZY) ? "fuzzy" : "typo");
	printf("mode=%s\n", filter_mode ? "filter" : "match");
	printf("runs=%" PRIu32 "\n", runs);
	printf("warmup=%" PRIu32 "\n", warmup);
	printf("avg_total_ns=%.0f\n", avg_ns);
	printf("avg_ns_per_item=%.2f\n", avg_ns_per_item);
	printf("matches_last_run=%zu\n", last_matches);
	printf("score_sum_last_run=%" PRId64 "\n", last_score_sum);

	match_query_destroy(&query);
	if (filter_mode) {
		string_ref_vec_destroy(&vec);
	}
	dataset_destroy(&ds);
	return 0;
}
