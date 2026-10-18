#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include <sys/stat.h>
#include <unistd.h>

#include "util/pipe.h"

#ifndef PATH_MAX
/* Some POSIX platforms omit PATH_MAX from limits.h. */
#define PATH_MAX 4096
#endif

/* C's os_process_pipe_create2 calls posix_spawn (not posix_spawnp) with
 * argv[0] as the path, so a name without a slash is resolved against the
 * current directory and never searched for on PATH. Each test runs in an
 * empty current directory with PATH set to a private directory that holds
 * a "helper" script. */
struct fixture {
	char cwd_before[PATH_MAX];
	char root[PATH_MAX];
	char path_dir[PATH_MAX];
	char child_cwd[PATH_MAX];
	char *old_path;
	bool had_path;
};

struct pipe_result {
	bool created;
	int status;
	size_t len;
	char payload[128];
};

static void write_script(const char *path, const char *body)
{
	FILE *f = fopen(path, "w");
	assert_non_null(f);
	assert_true(fputs(body, f) >= 0);
	assert_int_equal(fclose(f), 0);
	assert_int_equal(chmod(path, 0700), 0);
}

static void remove_file(const char *path)
{
	if (unlink(path) != 0 && errno != ENOENT)
		fail_msg("unlink failed for %s: %s", path, strerror(errno));
}

static void remove_dir(const char *path)
{
	if (rmdir(path) != 0 && errno != ENOENT)
		fail_msg("rmdir failed for %s: %s", path, strerror(errno));
}

static void join(char *out, const char *dir, const char *name)
{
	int n = snprintf(out, PATH_MAX, "%s/%s", dir, name);
	assert_true(n > 0 && n < PATH_MAX);
}

static int setup(void **state)
{
	struct fixture *fx = calloc(1, sizeof(*fx));
	assert_non_null(fx);
	char root_template[] = "pipe_exact_path_XXXXXX";
	char path[PATH_MAX];

	assert_non_null(getcwd(fx->cwd_before, sizeof(fx->cwd_before)));
	assert_non_null(mkdtemp(root_template));
	assert_non_null(realpath(root_template, fx->root));
	join(fx->path_dir, fx->root, "helper-path");
	join(fx->child_cwd, fx->root, "cwd");
	assert_int_equal(mkdir(fx->path_dir, 0700), 0);
	assert_int_equal(mkdir(fx->child_cwd, 0700), 0);

	join(path, fx->path_dir, "helper");
	write_script(path, "#!/bin/sh\nprintf PATH_HELPER\n");

	const char *old_path = getenv("PATH");
	fx->had_path = old_path != NULL;
	if (old_path) {
		fx->old_path = strdup(old_path);
		assert_non_null(fx->old_path);
	}
	assert_int_equal(setenv("PATH", fx->path_dir, 1), 0);
	assert_int_equal(chdir(fx->child_cwd), 0);

	*state = fx;
	return 0;
}

static int teardown(void **state)
{
	struct fixture *fx = *state;
	char path[PATH_MAX];

	assert_int_equal(chdir(fx->cwd_before), 0);
	if (fx->had_path)
		assert_int_equal(setenv("PATH", fx->old_path, 1), 0);
	else
		assert_int_equal(unsetenv("PATH"), 0);

	join(path, fx->child_cwd, "helper");
	remove_file(path);
	join(path, fx->child_cwd, "mysh");
	remove_file(path);
	join(path, fx->path_dir, "helper");
	remove_file(path);
	remove_dir(fx->child_cwd);
	remove_dir(fx->path_dir);
	remove_dir(fx->root);

	free(fx->old_path);
	free(fx);
	return 0;
}

static struct pipe_result run(struct os_process_args *args)
{
	struct pipe_result out = {0};
	os_process_pipe_t *pp = os_process_pipe_create2(args, "r");
	os_process_args_destroy(args);
	out.created = pp != NULL;
	out.status = -777;
	if (!pp)
		return out;

	while (out.len + 1 < sizeof(out.payload)) {
		size_t n =
			os_process_pipe_read(pp, (uint8_t *)out.payload + out.len, sizeof(out.payload) - 1 - out.len);
		if (!n)
			break;
		out.len += n;
	}
	out.payload[out.len] = 0;
	out.status = os_process_pipe_destroy(pp);
	return out;
}

static void test_create2_bare_helper_does_not_search_path(void **state)
{
	(void)state;
	struct pipe_result got = run(os_process_args_create("helper"));

	assert_false(got.created);
}

static void test_create2_bare_helper_runs_from_cwd(void **state)
{
	struct fixture *fx = *state;
	char path[PATH_MAX];
	join(path, fx->child_cwd, "helper");
	write_script(path, "#!/bin/sh\nprintf CWD_HELPER\n");

	struct pipe_result got = run(os_process_args_create("helper"));

	assert_true(got.created);
	assert_int_equal(got.status, 0);
	assert_string_equal(got.payload, "CWD_HELPER");
}

/* A script cannot observe its own argv[0] (the kernel passes the script
 * path to the interpreter), so run /bin/sh through a bare-named symlink
 * and have it print $0. */
static void test_create2_bare_name_keeps_argv0(void **state)
{
	struct fixture *fx = *state;
	char path[PATH_MAX];
	join(path, fx->child_cwd, "mysh");
	assert_int_equal(symlink("/bin/sh", path), 0);

	struct os_process_args *args = os_process_args_create("mysh");
	os_process_args_add_arg(args, "-c");
	os_process_args_add_arg(args, "printf %s \"$0\"");
	struct pipe_result got = run(args);

	assert_true(got.created);
	assert_int_equal(got.status, 0);
	assert_string_equal(got.payload, "mysh");
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(test_create2_bare_helper_does_not_search_path, setup, teardown),
		cmocka_unit_test_setup_teardown(test_create2_bare_helper_runs_from_cwd, setup, teardown),
		cmocka_unit_test_setup_teardown(test_create2_bare_name_keeps_argv0, setup, teardown),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
