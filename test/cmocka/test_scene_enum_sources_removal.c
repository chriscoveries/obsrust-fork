/* Lifetime conformance for the public source-enumeration paths that reach
 * scene_enum_sources(). Removing a later item while the callback is running
 * must not make the traversal visit an item that left the scene or read a
 * released item. Moving a later item into a group of the same scene takes it
 * out of the scene's list without removing it: it must not be passed to the
 * callback as a child of the scene (the group's own enumeration reaches it),
 * so a full-tree walk visits it once. The walk holds a reference to every item
 * it snapshots, and all of them must be released: once every owner has let
 * go, no source may still be referenced. An item being removed is already
 * marked removed while its deselect signal runs, before it leaves the scene's
 * list; a walk started from that signal must skip it. Each scenario runs in a
 * child so a sanitizer report or crash is isolated to its case. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <obs.h>
#include <util/base.h>

#define SOURCE_ID "scene_enum_sources_removal_source"
#define SOURCE_COUNT 4
#define CHILD_SKIP 77
#define CHILD_TIMEOUT 30

static void quiet_log(int lvl, const char *msg, va_list args, void *p)
{
	(void)lvl;
	(void)msg;
	(void)args;
	(void)p;
}

static const char *source_name(void *type_data)
{
	(void)type_data;
	return "enum source";
}

static void *source_create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	return source;
}

static void source_destroy(void *data)
{
	(void)data;
}

static uint32_t source_size(void *data)
{
	(void)data;
	return 1;
}

static bool start_obs(void)
{
	base_set_log_handler(quiet_log, NULL);
	if (!obs_startup("en-US", NULL, NULL))
		return false;

	struct obs_source_info info = {
		.id = SOURCE_ID,
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO,
		.get_name = source_name,
		.create = source_create,
		.destroy = source_destroy,
		.get_width = source_size,
		.get_height = source_size,
	};
	obs_register_source(&info);
	return true;
}

struct layout {
	obs_scene_t *scene;
	obs_source_t *sources[SOURCE_COUNT];
	obs_sceneitem_t *items[SOURCE_COUNT];
};

static void build_layout(struct layout *l)
{
	l->scene = obs_scene_create_private("enum source scene");
	for (size_t i = 0; i < SOURCE_COUNT; i++) {
		char name[24];
		snprintf(name, sizeof(name), "enum child %zu", i);
		l->sources[i] = obs_source_create_private(SOURCE_ID, name, NULL);
		l->items[i] = obs_scene_add(l->scene, l->sources[i]);
	}
}

struct walk {
	struct layout *layout;
	size_t remove_from;
	size_t remove_to;
	size_t visits[SOURCE_COUNT];
	size_t total;
	bool removed;
};

static void remove_later_sources(obs_source_t *parent, obs_source_t *child, void *param)
{
	struct walk *w = param;
	(void)parent;
	w->total++;
	for (size_t i = 0; i < SOURCE_COUNT; i++) {
		if (w->layout->sources[i] == child)
			w->visits[i]++;
	}

	if (!w->removed && child == w->layout->sources[0]) {
		w->removed = true;
		for (size_t i = w->remove_from; i < w->remove_to; i++)
			obs_sceneitem_remove(w->layout->items[i]);
	}
}

static int check_walk(const struct walk *w, const char *what)
{
	int ret = 0;
	for (size_t i = 0; i < SOURCE_COUNT; i++) {
		bool removed = i >= w->remove_from && i < w->remove_to;
		size_t want = removed ? 0 : 1;
		if (w->visits[i] != want) {
			fprintf(stderr, "%s: source %zu was passed %zu time(s), expected %zu\n", what, i, w->visits[i],
				want);
			ret = 1;
		}
	}
	size_t want_total = SOURCE_COUNT - (w->remove_to - w->remove_from);
	if (w->total != want_total) {
		fprintf(stderr, "%s: callback ran %zu time(s), expected %zu\n", what, w->total, want_total);
		ret = 1;
	}
	return ret;
}

static int scenario_active_sources(void)
{
	struct layout l;
	build_layout(&l);
	obs_source_t *root = obs_scene_get_source(l.scene);
	obs_source_inc_active(root);

	struct walk w = {.layout = &l, .remove_from = 1, .remove_to = 2};
	obs_source_enum_active_sources(root, remove_later_sources, &w);
	int ret = check_walk(&w, "obs_source_enum_active_sources");

	obs_source_dec_active(root);
	for (size_t i = 0; i < SOURCE_COUNT; i++)
		obs_source_release(l.sources[i]);
	obs_scene_release(l.scene);
	return ret;
}

static int scenario_full_tree(void)
{
	struct layout l;
	build_layout(&l);
	/* Keep removed items allocated so the old traversal deterministically
	 * exposes the stale-link behavior as an extra callback. */
	obs_sceneitem_addref(l.items[1]);
	obs_sceneitem_addref(l.items[2]);

	struct walk w = {.layout = &l, .remove_from = 1, .remove_to = 3};
	obs_source_enum_full_tree(obs_scene_get_source(l.scene), remove_later_sources, &w);
	int ret = check_walk(&w, "obs_source_enum_full_tree");

	obs_sceneitem_release(l.items[1]);
	obs_sceneitem_release(l.items[2]);
	for (size_t i = 0; i < SOURCE_COUNT; i++)
		obs_source_release(l.sources[i]);
	obs_scene_release(l.scene);
	return ret;
}

/* A callback that moves a later item into a group of the same scene. The
 * scene is [source 0, source 1, source 2, group]; source 2 is moved. */
struct reparent_walk {
	obs_source_t *scene_source;
	obs_source_t *sources[3];
	obs_sceneitem_t *moved_item;
	obs_sceneitem_t *group;
	size_t visits[3];
	size_t as_scene_child[3];
	bool moved;
};

static void move_later_source(obs_source_t *parent, obs_source_t *child, void *param)
{
	struct reparent_walk *w = param;

	for (size_t i = 0; i < 3; i++) {
		if (child == w->sources[i]) {
			w->visits[i]++;
			if (parent == w->scene_source)
				w->as_scene_child[i]++;
		}
	}
	if (!w->moved && child == w->sources[0]) {
		w->moved = true;
		obs_sceneitem_group_add_item(w->group, w->moved_item);
	}
}

static int scenario_reparent(bool full_tree)
{
	const char *what = full_tree ? "reparent, full tree" : "reparent, active sources";
	obs_scene_t *scene = obs_scene_create_private("reparent scene");
	struct reparent_walk w = {.scene_source = obs_scene_get_source(scene)};
	obs_sceneitem_t *items[3];

	for (size_t i = 0; i < 3; i++) {
		char name[24];
		snprintf(name, sizeof(name), "reparent child %zu", i);
		w.sources[i] = obs_source_create_private(SOURCE_ID, name, NULL);
		items[i] = obs_scene_add(scene, w.sources[i]);
	}
	w.moved_item = items[2];
	w.group = obs_scene_add_group(scene, "reparent group");

	if (full_tree) {
		obs_source_enum_full_tree(w.scene_source, move_later_source, &w);
	} else {
		obs_source_inc_active(w.scene_source);
		obs_source_enum_active_sources(w.scene_source, move_later_source, &w);
		obs_source_dec_active(w.scene_source);
	}

	int ret = 0;
	if (w.visits[0] != 1 || w.visits[1] != 1) {
		fprintf(stderr, "%s: sources 0 and 1 were passed %zu and %zu time(s), expected 1 each\n", what,
			w.visits[0], w.visits[1]);
		ret = 1;
	}
	if (w.as_scene_child[2] != 0) {
		fprintf(stderr, "%s: the moved source was passed as a child of the scene %zu time(s), expected 0\n",
			what, w.as_scene_child[2]);
		ret = 1;
	}
	if (full_tree && w.visits[2] != 1) {
		fprintf(stderr, "%s: the moved source was visited %zu time(s), expected 1 (through the group)\n", what,
			w.visits[2]);
		ret = 1;
	}

	for (size_t i = 0; i < 3; i++)
		obs_source_release(w.sources[i]);
	obs_scene_release(scene);
	return ret;
}

static int scenario_reparent_active_sources(void)
{
	return scenario_reparent(false);
}

static int scenario_reparent_full_tree(void)
{
	return scenario_reparent(true);
}

/* After the walk (which removed later items) and the removal of the rest, no
 * source may be referenced once its owner lets go. Items are removed
 * explicitly so that their destruction, which drops the source reference, is
 * synchronous; the weak references see a source whose last reference is gone. */
static int scenario_references_released(bool full_tree)
{
	const char *what = full_tree ? "references, full tree" : "references, active sources";
	struct layout l;
	build_layout(&l);
	obs_source_t *root = obs_scene_get_source(l.scene);

	struct walk w = {.layout = &l, .remove_from = 1, .remove_to = 3};
	if (full_tree) {
		obs_source_enum_full_tree(root, remove_later_sources, &w);
	} else {
		obs_source_inc_active(root);
		obs_source_enum_active_sources(root, remove_later_sources, &w);
		obs_source_dec_active(root);
	}

	/* the walk's callback already removed items remove_from..remove_to */
	for (size_t i = 0; i < SOURCE_COUNT; i++) {
		if (i < w.remove_from || i >= w.remove_to)
			obs_sceneitem_remove(l.items[i]);
	}

	obs_weak_source_t *weak[SOURCE_COUNT];
	for (size_t i = 0; i < SOURCE_COUNT; i++) {
		weak[i] = obs_source_get_weak_source(l.sources[i]);
		obs_source_release(l.sources[i]);
	}
	int ret = 0;
	for (size_t i = 0; i < SOURCE_COUNT; i++) {
		obs_source_t *still = obs_weak_source_get_source(weak[i]);
		if (still) {
			fprintf(stderr, "%s: source %zu is still referenced after every owner let go\n", what, i);
			obs_source_release(still);
			ret = 1;
		}
		obs_weak_source_release(weak[i]);
	}
	obs_scene_release(l.scene);
	return ret;
}

static int scenario_references_active_sources(void)
{
	return scenario_references_released(false);
}

static int scenario_references_full_tree(void)
{
	return scenario_references_released(true);
}

/* An item that is being removed is marked removed before its deselect signal
 * runs and is detached only afterwards. A walk started from that signal
 * must not pass its source to the callback. */
struct signal_walk {
	struct layout *layout;
	size_t signals;
	size_t removed_visits;
};

static void count_removed_source(obs_source_t *parent, obs_source_t *child, void *param)
{
	struct signal_walk *w = param;
	(void)parent;
	if (child == w->layout->sources[0])
		w->removed_visits++;
}

static void on_item_deselect(void *param, calldata_t *data)
{
	struct signal_walk *w = param;
	(void)data;
	w->signals++;
	obs_source_enum_full_tree(obs_scene_get_source(w->layout->scene), count_removed_source, w);
}

static int scenario_removal_signal_reentry(void)
{
	struct layout l;
	build_layout(&l);
	obs_sceneitem_select(l.items[0], true);

	struct signal_walk w = {.layout = &l};
	signal_handler_t *handler = obs_source_get_signal_handler(obs_scene_get_source(l.scene));
	signal_handler_connect(handler, "item_deselect", on_item_deselect, &w);
	obs_sceneitem_remove(l.items[0]);
	signal_handler_disconnect(handler, "item_deselect", on_item_deselect, &w);

	int ret = 0;
	if (w.signals != 1 || w.removed_visits != 0) {
		fprintf(stderr, "item_deselect re-entry: signals=%zu removed source visits=%zu, expected 1/0\n",
			w.signals, w.removed_visits);
		ret = 1;
	}
	for (size_t i = 0; i < SOURCE_COUNT; i++)
		obs_source_release(l.sources[i]);
	obs_scene_release(l.scene);
	return ret;
}

typedef int (*scenario_fn)(void);

static void run_in_child(scenario_fn fn, const char *what)
{
	fflush(stderr);
	pid_t pid = fork();
	assert_true(pid >= 0);
	if (pid == 0) {
		/* cmocka's own handlers would carry on running the group in the
		 * child after a crash; die instead so the parent reports it */
		signal(SIGSEGV, SIG_DFL);
		signal(SIGBUS, SIG_DFL);
		signal(SIGABRT, SIG_DFL);
		signal(SIGILL, SIG_DFL);
		signal(SIGFPE, SIG_DFL);
		alarm(CHILD_TIMEOUT);
		if (!start_obs())
			_exit(CHILD_SKIP);
		int ret = fn();
		fflush(stderr);
		_exit(ret);
	}

	int status = 0;
	assert_int_equal(waitpid(pid, &status, 0), pid);
	if (WIFEXITED(status) && WEXITSTATUS(status) == CHILD_SKIP)
		skip();
	if (WIFSIGNALED(status)) {
		fprintf(stderr, "%s: child died from signal %d (%s)\n", what, WTERMSIG(status),
			WTERMSIG(status) == SIGALRM ? "hang" : "crash");
		fail();
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "%s failed\n", what);
		fail();
	}
}

static void active_sources_removal(void **state)
{
	(void)state;
	run_in_child(scenario_active_sources, "active sources removal");
}

static void full_tree_removal(void **state)
{
	(void)state;
	run_in_child(scenario_full_tree, "full tree removal");
}

static void reparent_active_sources(void **state)
{
	(void)state;
	run_in_child(scenario_reparent_active_sources, "reparent, active sources");
}

static void reparent_full_tree(void **state)
{
	(void)state;
	run_in_child(scenario_reparent_full_tree, "reparent, full tree");
}

static void references_released_active_sources(void **state)
{
	(void)state;
	run_in_child(scenario_references_active_sources, "references, active sources");
}

static void references_released_full_tree(void **state)
{
	(void)state;
	run_in_child(scenario_references_full_tree, "references, full tree");
}

static void removal_signal_reentry(void **state)
{
	(void)state;
	run_in_child(scenario_removal_signal_reentry, "item_deselect re-entry");
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(active_sources_removal),
		cmocka_unit_test(full_tree_removal),
		cmocka_unit_test(reparent_active_sources),
		cmocka_unit_test(reparent_full_tree),
		cmocka_unit_test(references_released_active_sources),
		cmocka_unit_test(references_released_full_tree),
		cmocka_unit_test(removal_signal_reentry),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
