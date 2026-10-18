#include <locale.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>

#include <cmocka.h>

#include "util/bmem.h"
#include "util/config-file.h"

struct hex_case {
	const char *name;
	const char *value;
	uint64_t expected_bits;
};

static uint64_t double_bits(double value)
{
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

static void check_cases(const struct hex_case *cases, size_t count)
{
	config_t *config = NULL;
	size_t mismatches = 0;
	assert_int_equal(config_open_string(&config, ""), CONFIG_SUCCESS);
	assert_non_null(config);

	for (size_t i = 0; i < count; i++) {
		config_set_string(config, "Hex", cases[i].name, cases[i].value);
		config_set_default_string(config, "Hex", cases[i].name, cases[i].value);
	}
	for (size_t i = 0; i < count; i++) {
		uint64_t user_bits = double_bits(config_get_double(config, "Hex", cases[i].name));
		uint64_t default_bits = double_bits(config_get_default_double(config, "Hex", cases[i].name));
		if (user_bits != cases[i].expected_bits || default_bits != cases[i].expected_bits) {
			print_error("%s (\"%s\"): user=%016llx default=%016llx expected=%016llx\n", cases[i].name,
				    cases[i].value, (unsigned long long)user_bits, (unsigned long long)default_bits,
				    (unsigned long long)cases[i].expected_bits);
			mismatches++;
		}
	}
	config_close(config);
	assert_int_equal(mismatches, 0);
}

static void test_hex_float_values_match_strtod(void **state)
{
	(void)state;
	static const struct hex_case cases[] = {
		{"min_subnormal", "0x1p-1074", UINT64_C(0x0000000000000001)},
		{"min_normal", "0x10000000000000p-1074", UINT64_C(0x0010000000000000)},
		{"sticky_rounding", "0x1.000000000000080000000000000000001p0", UINT64_C(0x3ff0000000000001)},
		{"tie_to_even_down", "0x1.00000000000008p0", UINT64_C(0x3ff0000000000000)},
		{"tie_to_even_up", "0x1.00000000000018p0", UINT64_C(0x3ff0000000000002)},
		{"half_min_subnormal", "0x1p-1075", UINT64_C(0x0000000000000000)},
		{"control", "0x1p-1023", UINT64_C(0x0008000000000000)},
	};
	check_cases(cases, sizeof(cases) / sizeof(cases[0]));
}

/* os_strtod swaps '.' for the locale's decimal point before strtod, so a
 * '.'-spelled value parses the same under a comma-decimal LC_NUMERIC. */
static void test_hex_float_values_ignore_comma_locale(void **state)
{
	(void)state;
	static const char *const comma_locales[] = {
		"German_Germany.1252", "French_France.1252", "Dutch_Netherlands.1252",
		"de_DE.UTF-8",         "de_DE.utf8",         "de_DE",
		"fr_FR.UTF-8",         "fr_FR.utf8",         "fr_FR",
		"nl_NL.UTF-8",         "nl_NL.utf8",
	};
	static const struct hex_case cases[] = {
		{"fraction", "0x1.8p0", UINT64_C(0x3ff8000000000000)},
		{"trailing_point", "0x1.p1", UINT64_C(0x4000000000000000)},
		{"leading_point", "0x.8p1", UINT64_C(0x3ff0000000000000)},
		{"overflow", "0x1.fffffffffffff8p1023", UINT64_C(0x7ff0000000000000)},
		{"sticky_rounding", "0x1.000000000000080000000000000000001p0", UINT64_C(0x3ff0000000000001)},
		{"tie_to_even_up", "0x1.00000000000018p0", UINT64_C(0x3ff0000000000002)},
		{"locale_point", "0x1,8p0", UINT64_C(0x3ff8000000000000)},
		{"locale_leading_point", "0x,8p1", UINT64_C(0x3ff0000000000000)},
		{"two_points", "0x1.8.8p0", UINT64_C(0x3ff8000000000000)},
		{"decimal", "1.5", UINT64_C(0x3ff8000000000000)},
	};
	const char *current = setlocale(LC_NUMERIC, NULL);
	char *saved = current ? bstrdup(current) : NULL;
	const char *found = NULL;

	assert_non_null(saved);
	for (size_t i = 0; i < sizeof(comma_locales) / sizeof(comma_locales[0]) && !found; i++) {
		if (setlocale(LC_NUMERIC, comma_locales[i]) && localeconv()->decimal_point[0] == ',')
			found = comma_locales[i];
	}
	if (!found) {
		setlocale(LC_NUMERIC, saved);
		bfree(saved);
		print_message("no comma-decimal LC_NUMERIC locale (de_DE, fr_FR, nl_NL) is installed; skipping\n");
		skip();
	}

	check_cases(cases, sizeof(cases) / sizeof(cases[0]));
	setlocale(LC_NUMERIC, saved);
	bfree(saved);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_hex_float_values_match_strtod),
		cmocka_unit_test(test_hex_float_values_ignore_comma_locale),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
