#ifndef MATCHING_H
#define MATCHING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum matching_algorithm {
	MATCHING_ALGORITHM_NORMAL,
	MATCHING_ALGORITHM_PREFIX,
	MATCHING_ALGORITHM_FUZZY,
	MATCHING_ALGORITHM_TYPO
};

struct match_query_word {
	const char *word;
	char *ascii_lower;
	uint32_t *utf32_lower;
	size_t len;
	bool ascii;
};

struct match_query {
	char *normalized;
	struct match_query_word *words;
	size_t word_count;
};

struct match_query match_query_create(const char *patterns);
void match_query_destroy(struct match_query *query);
int32_t match_query_words(enum matching_algorithm algorithm, const struct match_query *query, const char *str);

int32_t match_words(enum matching_algorithm algorithm, const char *restrict patterns, const char *restrict str);

#endif /* MATCHING_H */
