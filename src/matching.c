#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "matching.h"
#include "unicode.h"
#include "xmalloc.h"

#undef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static int32_t simple_match_query(
		const struct match_query *query,
		const char *restrict str);

static int32_t prefix_match_query(
		const struct match_query *query,
		const char *restrict str);

static int32_t fuzzy_match_query(
		const struct match_query *query,
		const char *restrict str);

static int32_t typo_match_query(
		const struct match_query *query,
		const char *restrict str);

static int32_t typo_match_word(
		const struct match_query_word *word,
		const char *restrict str,
		bool str_ascii);

static int32_t fuzzy_match(
		const char *restrict pattern,
		const char *restrict str);

static int32_t fuzzy_match_recurse(
		const char *restrict pattern,
		const char *restrict str,
		int32_t score,
		bool first_match_only,
		bool first_char);

static int32_t compute_score(
		int32_t jump,
		bool first_char,
		const char *restrict match);

static bool is_ascii_string(const char *restrict s)
{
	const unsigned char *p = (const unsigned char *)s;
	while (*p != '\0') {
		if (*p >= 0x80) {
			return false;
		}
		p++;
	}
	return true;
}

static struct match_query_word match_query_word_create(const char *restrict word)
{
	struct match_query_word w = {
		.word = word,
		.ascii_lower = NULL,
		.utf32_lower = NULL,
		.len = 0,
		.ascii = true,
	};

	for (const unsigned char *p = (const unsigned char *)word; *p != '\0'; p++) {
		if (*p >= 0x80) {
			w.ascii = false;
			break;
		}
	}

	if (w.ascii) {
		w.len = strlen(word);
		w.ascii_lower = xmalloc(w.len + 1);
		for (size_t i = 0; i < w.len; i++) {
			w.ascii_lower[i] = (char)tolower((unsigned char)word[i]);
		}
		w.ascii_lower[w.len] = '\0';

		w.utf32_lower = xcalloc(w.len + 1, sizeof(*w.utf32_lower));
		for (size_t i = 0; i < w.len; i++) {
			w.utf32_lower[i] = (uint32_t)w.ascii_lower[i];
		}
		w.utf32_lower[w.len] = U'\0';
	} else {
		w.utf32_lower = utf8_string_to_utf32_string(word);
		w.len = utf32_strlen(w.utf32_lower);
		for (size_t i = 0; i < w.len; i++) {
			w.utf32_lower[i] = utf32_tolower(w.utf32_lower[i]);
		}
	}

	return w;
}

struct match_query match_query_create(const char *patterns)
{
	struct match_query query = {
		.normalized = NULL,
		.words = NULL,
		.word_count = 0,
	};

	query.normalized = utf8_normalize(patterns);
	if (query.normalized == NULL) {
		query.normalized = xstrdup(patterns);
	}

	size_t words_cap = 4;
	query.words = xcalloc(words_cap, sizeof(*query.words));

	char *p = query.normalized;
	while (*p != '\0') {
		while (*p == ' ') {
			p++;
		}
		if (*p == '\0') {
			break;
		}
		char *start = p;
		while (*p != '\0' && *p != ' ') {
			p++;
		}
		if (*p == ' ') {
			*p = '\0';
			p++;
		}

		if (*start == '\0') {
			continue;
		}

		if (query.word_count == words_cap) {
			words_cap *= 2;
			query.words = xrealloc(query.words, words_cap * sizeof(*query.words));
		}
		query.words[query.word_count++] = match_query_word_create(start);
	}

	return query;
}

void match_query_destroy(struct match_query *query)
{
	if (query == NULL) {
		return;
	}
	for (size_t i = 0; i < query->word_count; i++) {
		free(query->words[i].ascii_lower);
		free(query->words[i].utf32_lower);
	}
	free(query->words);
	free(query->normalized);

	query->normalized = NULL;
	query->words = NULL;
	query->word_count = 0;
}

int32_t match_query_words(enum matching_algorithm algorithm, const struct match_query *query, const char *str)
{
	switch (algorithm) {
		case MATCHING_ALGORITHM_NORMAL:
			return simple_match_query(query, str);
		case MATCHING_ALGORITHM_PREFIX:
			return prefix_match_query(query, str);
		case MATCHING_ALGORITHM_FUZZY:
			return fuzzy_match_query(query, str);
		case MATCHING_ALGORITHM_TYPO:
			return typo_match_query(query, str);
		default:
			return INT32_MIN;
	}
}

/*
 * Select the appropriate algorithm, and return its score.
 * Each algorithm returns larger scores for better matches,
 * and returns INT32_MIN if a word is not found.
 */
int32_t match_words(
		enum matching_algorithm algorithm,
		const char *restrict patterns,
		const char *restrict str)
{
	struct match_query query = match_query_create(patterns);
	int32_t score = match_query_words(algorithm, &query, str);
	match_query_destroy(&query);
	return score;
}

/*
 * Perform simple matching against str for each word in query.
 * Returns the negative sum of substring distances from the start of str.
 * If a word is not found, returns INT32_MIN.
 */
int32_t simple_match_query(const struct match_query *query, const char *restrict str)
{
	int32_t score = 0;
	for (size_t i = 0; i < query->word_count; i++) {
		char *c = utf8_strcasestr(str, query->words[i].word);
		if (c == NULL) {
			return INT32_MIN;
		}
		score -= c - str;
	}
	return score;
}

/*
 * Perform prefix matching against str for each word in query.
 * Returns the negative sum of remaining string suffix lengths.
 * If a word is not found, returns INT32_MIN.
 */
int32_t prefix_match_query(const struct match_query *query, const char *restrict str)
{
	int32_t score = 0;
	for (size_t i = 0; i < query->word_count; i++) {
		const char *pattern = query->words[i].word;
		char *c = utf8_strcasestr(str, pattern);
		if (c != str) {
			return INT32_MIN;
		}
		score -= utf8_strlen(str) - utf8_strlen(pattern);
	}
	return score;
}


/*
 * Return the sum of fuzzy_match(word, str) for each word in query.
 * If a word is not found, returns INT32_MIN.
 */
int32_t fuzzy_match_query(const struct match_query *query, const char *restrict str)
{
	int32_t score = 0;
	for (size_t i = 0; i < query->word_count; i++) {
		int32_t word_score = fuzzy_match(query->words[i].word, str);
		if (word_score == INT32_MIN) {
			return INT32_MIN;
		}
		score += word_score;
	}
	return score;
}

static int32_t typo_match_query(const struct match_query *query, const char *restrict str)
{
	int32_t score = 0;
	bool str_ascii = is_ascii_string(str);
	for (size_t i = 0; i < query->word_count; i++) {
		int32_t word_score = typo_match_word(&query->words[i], str, str_ascii);
		if (word_score == INT32_MIN) {
			return INT32_MIN;
		}
		score += word_score;
	}
	return score;
}

static int32_t typo_match_word_osa_substring_ascii(
		const char *restrict pattern_lower,
		size_t plen,
		const char *restrict str,
		uint8_t max_dist,
		uint32_t *best_start_out)
{
	if (plen == 0) {
		*best_start_out = 0;
		return 0;
	}

	const uint8_t sat = (uint8_t)(max_dist + 1);

	uint8_t prev_buf[plen + 1];
	uint8_t curr_buf[plen + 1];
	uint8_t prev2_buf[plen + 1];

	uint32_t prev_start_buf[plen + 1];
	uint32_t curr_start_buf[plen + 1];
	uint32_t prev2_start_buf[plen + 1];

	uint8_t *prev = prev_buf;
	uint8_t *curr = curr_buf;
	uint8_t *prev2 = prev2_buf;

	uint32_t *prev_start = prev_start_buf;
	uint32_t *curr_start = curr_start_buf;
	uint32_t *prev2_start = prev2_start_buf;

	for (size_t i = 0; i <= plen; i++) {
		prev[i] = (i > sat) ? sat : (uint8_t)i;
		prev_start[i] = 0;
		prev2[i] = prev[i];
		prev2_start[i] = 0;
	}

	uint8_t best_cost = sat;
	uint32_t best_start = 0;

	uint32_t j = 0;
	uint32_t prev_text = 0;

	for (const unsigned char *p = (const unsigned char *)str; *p != '\0'; p++) {
		j++;
		uint32_t text = (uint32_t)tolower(*p);

		curr[0] = 0;
		curr_start[0] = j;

		for (size_t i = 1; i <= plen; i++) {
			uint8_t best = sat;
			uint32_t best_s = 0;

			/* Deletion */
			uint8_t del = curr[i - 1];
			del = (del >= sat) ? sat : (uint8_t)(del + 1);
			best = del;
			best_s = curr_start[i - 1];

			/* Insertion */
			uint8_t ins = prev[i];
			ins = (ins >= sat) ? sat : (uint8_t)(ins + 1);
			if (ins < best || (ins == best && prev_start[i] < best_s)) {
				best = ins;
				best_s = prev_start[i];
			}

			/* Substitution / match */
			uint8_t sub = prev[i - 1];
			uint8_t cost = (pattern_lower[i - 1] == (char)text) ? 0 : 1;
			sub = (sub >= sat) ? sat : (uint8_t)(sub + cost);
			if (sub < best || (sub == best && prev_start[i - 1] < best_s)) {
				best = sub;
				best_s = prev_start[i - 1];
			}

			/* Transposition */
			if (j > 1 && i > 1 && (uint32_t)pattern_lower[i - 1] == prev_text &&
					(uint32_t)pattern_lower[i - 2] == text) {
				uint8_t trans = prev2[i - 2];
				trans = (trans >= sat) ? sat : (uint8_t)(trans + 1);
				if (trans < best || (trans == best && prev2_start[i - 2] < best_s)) {
					best = trans;
					best_s = prev2_start[i - 2];
				}
			}

			curr[i] = best;
			curr_start[i] = best_s;
		}

		if (curr[plen] < best_cost) {
			best_cost = curr[plen];
			best_start = curr_start[plen];
		} else if (curr[plen] == best_cost && curr_start[plen] < best_start) {
			best_start = curr_start[plen];
		}

		if (best_cost == 0) {
			break;
		}

		uint8_t *tmp = prev2;
		prev2 = prev;
		prev = curr;
		curr = tmp;

		uint32_t *tmp_start = prev2_start;
		prev2_start = prev_start;
		prev_start = curr_start;
		curr_start = tmp_start;

		prev_text = text;
	}

	*best_start_out = best_start;
	return best_cost;
}

static int32_t typo_match_word_osa_substring_utf8(
		const uint32_t *restrict pattern_lower,
		size_t plen,
		const char *restrict str,
		uint8_t max_dist,
		uint32_t *best_start_out)
{
	if (plen == 0) {
		*best_start_out = 0;
		return 0;
	}

	const uint8_t sat = (uint8_t)(max_dist + 1);

	uint8_t prev_buf[plen + 1];
	uint8_t curr_buf[plen + 1];
	uint8_t prev2_buf[plen + 1];

	uint32_t prev_start_buf[plen + 1];
	uint32_t curr_start_buf[plen + 1];
	uint32_t prev2_start_buf[plen + 1];

	uint8_t *prev = prev_buf;
	uint8_t *curr = curr_buf;
	uint8_t *prev2 = prev2_buf;

	uint32_t *prev_start = prev_start_buf;
	uint32_t *curr_start = curr_start_buf;
	uint32_t *prev2_start = prev2_start_buf;

	for (size_t i = 0; i <= plen; i++) {
		prev[i] = (i > sat) ? sat : (uint8_t)i;
		prev_start[i] = 0;
		prev2[i] = prev[i];
		prev2_start[i] = 0;
	}

	uint8_t best_cost = sat;
	uint32_t best_start = 0;

	uint32_t j = 0;
	uint32_t prev_text = 0;

	const char *p = str;
	while (*p != '\0') {
		j++;
		uint32_t text = utf32_tolower(utf8_to_utf32(p));

		curr[0] = 0;
		curr_start[0] = j;

		for (size_t i = 1; i <= plen; i++) {
			uint8_t best = sat;
			uint32_t best_s = 0;

			/* Deletion */
			uint8_t del = curr[i - 1];
			del = (del >= sat) ? sat : (uint8_t)(del + 1);
			best = del;
			best_s = curr_start[i - 1];

			/* Insertion */
			uint8_t ins = prev[i];
			ins = (ins >= sat) ? sat : (uint8_t)(ins + 1);
			if (ins < best || (ins == best && prev_start[i] < best_s)) {
				best = ins;
				best_s = prev_start[i];
			}

			/* Substitution / match */
			uint8_t sub = prev[i - 1];
			uint8_t cost = (pattern_lower[i - 1] == text) ? 0 : 1;
			sub = (sub >= sat) ? sat : (uint8_t)(sub + cost);
			if (sub < best || (sub == best && prev_start[i - 1] < best_s)) {
				best = sub;
				best_s = prev_start[i - 1];
			}

			/* Transposition */
			if (j > 1 && i > 1 && pattern_lower[i - 1] == prev_text && pattern_lower[i - 2] == text) {
				uint8_t trans = prev2[i - 2];
				trans = (trans >= sat) ? sat : (uint8_t)(trans + 1);
				if (trans < best || (trans == best && prev2_start[i - 2] < best_s)) {
					best = trans;
					best_s = prev2_start[i - 2];
				}
			}

			curr[i] = best;
			curr_start[i] = best_s;
		}

		if (curr[plen] < best_cost) {
			best_cost = curr[plen];
			best_start = curr_start[plen];
		} else if (curr[plen] == best_cost && curr_start[plen] < best_start) {
			best_start = curr_start[plen];
		}

		if (best_cost == 0) {
			break;
		}

		uint8_t *tmp = prev2;
		prev2 = prev;
		prev = curr;
		curr = tmp;

		uint32_t *tmp_start = prev2_start;
		prev2_start = prev_start;
		prev_start = curr_start;
		curr_start = tmp_start;

		prev_text = text;
		p = utf8_next_char(p);
	}

	*best_start_out = best_start;
	return best_cost;
}

static int32_t typo_match_word(const struct match_query_word *word, const char *restrict str, bool str_ascii)
{
	const uint8_t max_dist = 2;
	if (word->len == 0) {
		return 0;
	}

	uint32_t best_start = 0;
	uint32_t cost;

	if (word->ascii && str_ascii) {
		cost = (uint32_t)typo_match_word_osa_substring_ascii(
				word->ascii_lower,
				word->len,
				str,
				max_dist,
				&best_start);
	} else {
		cost = (uint32_t)typo_match_word_osa_substring_utf8(
				word->utf32_lower,
				word->len,
				str,
				max_dist,
				&best_start);
	}

	if (cost > max_dist || cost >= word->len) {
		return INT32_MIN;
	}

	const int32_t cost_penalty = 100;
	return -((int32_t)best_start) - (int32_t)cost * cost_penalty;
}

/*
 * Returns score if each character in pattern is found sequentially within str.
 * Returns INT32_MIN otherwise.
 */
int32_t fuzzy_match(const char *restrict pattern, const char *restrict str)
{
	const int unmatched_letter_penalty = -1;
	const size_t slen = utf8_strlen(str);
	const size_t plen = utf8_strlen(pattern);
	int32_t score = 0;

	if (*pattern == '\0') {
		return score;
	}
	if (slen < plen) {
		return INT32_MIN;
	}

	/* We can already penalise any unused letters. */
	score += unmatched_letter_penalty * (int32_t)(slen - plen);

        /*
         * If the string is more than 100 characters, just find the first fuzzy
         * match rather than the best.
         *
         * This is required as the number of possible matches (for patterns and
         * strings all consisting of one letter) scales something like:
         *
         * slen! / (plen! (slen - plen)!)  ~  slen^plen for plen << slen
         *
         * This quickly grinds everything to a halt. 100 is chosen fairly
         * arbitrarily from the following logic:
         *
         * - e is the most common character in English, at around 13% of
         *   letters. Depending on the context, let's say this be up to 20%.
         * - 100 * 0.20 = 20 repeats of the same character.
         * - In the worst case here, 20! / (10! 10!) ~200,000 possible matches,
         *   which is "slow but not frozen" for my machine.
         *
         * In reality, this worst case shouldn't be hit, and finding the "best"
         * fuzzy match in lines of text > 100 characters isn't really in scope
         * for a dmenu clone.
         */
        bool first_match_only = slen > 100;

	/* Perform the match. */
	score = fuzzy_match_recurse(pattern, str, score, first_match_only, true);

	return score;
}

/*
 * Recursively match the whole of pattern against str.
 * The score parameter is the score of the previously matched character.
 *
 * This reaches a maximum recursion depth of strlen(pattern) + 1. However, the
 * stack usage is small (the maximum I've seen on x86_64 is 144 bytes with
 * gcc -O3), so this shouldn't matter unless pattern contains thousands of
 * characters.
 */
int32_t fuzzy_match_recurse(
		const char *restrict pattern,
		const char *restrict str,
		int32_t score,
		bool first_match_only,
		bool first_char)
{
	if (*pattern == '\0') {
		/* We've matched the full pattern. */
		return score;
	}

	const char *match = str;
	uint32_t search = utf8_to_utf32(pattern);

	int32_t best_score = INT32_MIN;

	/*
	 * Find all occurrences of the next pattern character in str, and
	 * recurse on them.
	 */
	while ((match = utf8_strcasechr(match, search)) != NULL) {
		int32_t jump = 0;
		for (const char *tmp = str; tmp != match; tmp = utf8_next_char(tmp)) {
			jump++;
		}
		int32_t subscore = fuzzy_match_recurse(
				utf8_next_char(pattern),
				utf8_next_char(match),
				compute_score(jump, first_char, match),
				first_match_only,
				false);
		best_score = MAX(best_score, subscore);
		match = utf8_next_char(match);

		if (first_match_only) {
			break;
		}
	}

	if (best_score == INT32_MIN) {
		/* We couldn't match the rest of the pattern. */
		return INT32_MIN;
	} else {
		return score + best_score;
	}
}

/*
 * Calculate the score for a single matching letter.
 * The scoring system is taken from fts_fuzzy_match v0.2.0 by Forrest Smith,
 * which is licensed to the public domain.
 *
 * The factors affecting score are:
 *   - Bonuses:
 *     - If there are multiple adjacent matches.
 *     - If a match occurs after a separator character.
 *     - If a match is uppercase, and the previous character is lowercase.
 *
 *   - Penalties:
 *     - If there are letters before the first match.
 *     - If there are superfluous characters in str (already accounted for).
 */
int32_t compute_score(int32_t jump, bool first_char, const char *restrict match)
{
	const int adjacency_bonus = 15;
	const int separator_bonus = 30;
	const int camel_bonus = 30;
	const int first_letter_bonus = 15;

	const int leading_letter_penalty = -5;
	const int max_leading_letter_penalty = -15;

	int32_t score = 0;

	const uint32_t cur = utf8_to_utf32(match);

	/* Apply bonuses. */
	if (!first_char && jump == 0) {
		score += adjacency_bonus;
	}
	if (!first_char || jump > 0) {
		const uint32_t prev = utf8_to_utf32(utf8_prev_char(match));
		if (utf32_isupper(cur) && utf32_islower(prev)) {
			score += camel_bonus;
		}
		if (utf32_isalnum(cur) && !utf32_isalnum(prev)) {
			score += separator_bonus;
		}
	}
	if (first_char && jump == 0) {
		/* Match at start of string gets separator bonus. */
		score += first_letter_bonus;
	}

	/* Apply penalties. */
	if (first_char) {
		score += MAX(leading_letter_penalty * jump,
				max_leading_letter_penalty);
	}

	return score;
}
