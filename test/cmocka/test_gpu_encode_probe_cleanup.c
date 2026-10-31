#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>

#include <cmocka.h>
#include <pthread.h>

#include "../../libobs/obs-gpu-encode-probe.h"

struct probe_test_state {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	pthread_t worker;
	bool stop_requested;
	bool worker_exited;
	bool worker_joined;
	bool resources_freed;
	bool sample_supported;
	unsigned null_probe_calls;
	unsigned sample_probe_calls;
	unsigned fallback_calls;
	unsigned raw_fallback_calls;
	unsigned raw_encode_calls;
};

static void *probe_worker(void *data)
{
	struct probe_test_state *state = data;
	pthread_mutex_lock(&state->mutex);
	while (!state->stop_requested)
		pthread_cond_wait(&state->cond, &state->mutex);
	state->worker_exited = true;
	pthread_mutex_unlock(&state->mutex);
	return NULL;
}

static void start_probe_worker(struct probe_test_state *state)
{
	assert_int_equal(pthread_mutex_init(&state->mutex, NULL), 0);
	assert_int_equal(pthread_cond_init(&state->cond, NULL), 0);
	state->stop_requested = false;
	state->worker_exited = false;
	state->worker_joined = false;
	state->resources_freed = false;
	assert_int_equal(pthread_create(&state->worker, NULL, probe_worker, state), 0);
}

static bool test_can_encode(void *data, const void *sample)
{
	struct probe_test_state *state = data;
	return !sample || state->sample_supported;
}

static bool probe_sample(void *data)
{
	struct probe_test_state *state = data;
	assert_true(test_can_encode(state, NULL));
	state->null_probe_calls++;
	assert_int_equal(test_can_encode(state, state), state->sample_supported);
	state->sample_probe_calls++;
	return state->sample_supported;
}

static void select_raw_fallback(void *data)
{
	struct probe_test_state *state = data;
	state->fallback_calls++;
}

static void stop_probe_worker(void *data)
{
	struct probe_test_state *state = data;
	pthread_mutex_lock(&state->mutex);
	state->stop_requested = true;
	pthread_cond_broadcast(&state->cond);
	pthread_mutex_unlock(&state->mutex);
	assert_int_equal(pthread_join(state->worker, NULL), 0);
	state->worker_joined = true;
}

static void free_probe_resources(void *data)
{
	struct probe_test_state *state = data;
	assert_true(state->worker_joined);
	state->resources_freed = true;
	pthread_cond_destroy(&state->cond);
	pthread_mutex_destroy(&state->mutex);
}

static bool raw_fallback_encode(struct probe_test_state *state)
{
	assert_int_equal(state->fallback_calls, 1);
	state->raw_fallback_calls++;
	return true;
}

static bool raw_encode(struct probe_test_state *state)
{
	state->raw_encode_calls++;
	return true;
}

static void test_failed_sample_probe_joins_worker_before_free_and_restarts_raw(void **data)
{
	(void)data;
	struct probe_test_state state = {0};
	const struct obs_gpu_encode_probe_ops ops = {
		.sample_probe = probe_sample,
		.fallback = select_raw_fallback,
		.stop_worker = stop_probe_worker,
		.free_resources = free_probe_resources,
	};

	start_probe_worker(&state);
	assert_false(obs_gpu_encode_probe_and_cleanup(true, true, true, &ops, &state));
	assert_int_equal(state.null_probe_calls, 1);
	assert_int_equal(state.sample_probe_calls, 1);
	assert_int_equal(state.fallback_calls, 1);
	assert_true(state.worker_exited);
	assert_true(state.worker_joined);
	assert_true(state.resources_freed);
	assert_true(raw_fallback_encode(&state));

	/* Restart takes the synchronous raw route, then shuts down cleanly. */
	start_probe_worker(&state);
	assert_true(obs_gpu_encode_probe_and_cleanup(true, false, true, &ops, &state));
	assert_int_equal(state.sample_probe_calls, 1);
	assert_true(raw_encode(&state));
	stop_probe_worker(&state);
	free_probe_resources(&state);
	assert_int_equal(state.raw_fallback_calls, 1);
	assert_int_equal(state.raw_encode_calls, 1);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_failed_sample_probe_joins_worker_before_free_and_restarts_raw),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
