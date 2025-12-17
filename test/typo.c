#include <locale.h>
#include <stdlib.h>

#include "matching.h"
#include "tap.h"

static void is_match(const char *pattern, const char *str, const char *message)
{
	int32_t res = match_words(MATCHING_ALGORITHM_TYPO, pattern, str);
	tap_isnt(res, INT32_MIN, message);
}

static void isnt_match(const char *pattern, const char *str, const char *message)
{
	int32_t res = match_words(MATCHING_ALGORITHM_TYPO, pattern, str);
	tap_is(res, INT32_MIN, message);
}

int main(int argc, char *argv[])
{
	setlocale(LC_ALL, "");

	tap_version(14);

	is_match("chrmoe", "chrome", "Transposition within threshold");
	is_match("firefocx", "firefox", "Deletion within threshold");
	is_match("firefocx browser", "Firefox Browser", "Multi-word query with typo");

	isnt_match("abcdef", "ghijkl", "Too many edits");
	isnt_match("a", "b", "Single-letter typo does not match");
	isnt_match("ab", "cd", "Two-letter completely different does not match");

	is_match("ab", "ac", "Two-letter single substitution matches");

	tap_plan();

	return EXIT_SUCCESS;
}

