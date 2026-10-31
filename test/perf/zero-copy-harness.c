/* Windowless zero-copy measurement harness (fork #47, M1/M2 epic).
 *
 * Boots OBS with no NSApplication, no window and no display: Metal video,
 * one color scene, a VideoToolbox encoder into a null output. Runs until
 * VideoToolbox accepts N submissions, then prints the profiler
 * `download_frame` count and counted source-plane copy bytes.
 *
 * Usage: zero-copy-harness [frames=120] [--warmup=N] [--raw-fallback|--raw-planar|--raw-no-gpu]
 * `--raw-fallback` keeps GPU conversion enabled but forces the encoder's raw
 * input route. `--raw-planar` also turns off the renderer's NV12 textures
 * (OBS_METAL_DISABLE_NV12), so libobs converts into separate plane textures
 * as it does with master's Metal renderer, which has no NV12 textures.
 * `--raw-no-gpu` exercises legacy CPU input (`--raw` alias).
 *
 * Timing (#755): the first N accepted submissions (default frames / 5) are
 * warm-up. Over the rest, the plugin reads process user + system CPU time at
 * submission N and at the last one, and records each later frame's latency
 * from entering the encode callback, and from the VideoToolbox submit, to
 * the VideoToolbox output callback. A second `timing` line prints them.
 *
 * This is a CLI measurement tool, not a cmocka unit test: it needs a full
 * build tree (graphics + encoder + output modules) and real GPU/VideoToolbox.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>
#include <libproc.h>
#include <sys/time.h>

#include <util/base.h>
#include <util/platform.h>
#include <util/profiler.h>
#include <obs.h>

#define DEFAULT_FRAMES 120
#define TIMEOUT_MS 120000
#define NV12_1080P_BYTES UINT64_C(3110400)

struct wait_state {
	uint64_t waits;
};

static char vt_plugin_binary[1152] = {0};
static void *vt_plugin_handle = NULL;

/* Byte counter living in the loaded mac-videotoolbox plugin (see
 * vt-frame-copy.h). Resolved with dlopen(RTLD_NOLOAD) so the harness
 * reads the same counter the encoder bumps. Missing symbols are fatal so
 * the harness cannot report a false zero. */
static void (*vt_copy_bytes_reset_fn)(void) = NULL;
static uint64_t (*vt_copy_bytes_total_fn)(void) = NULL;
static void (*vt_encode_submission_reset_fn)(uint64_t) = NULL;
static uint64_t (*vt_encode_submission_total_fn)(void) = NULL;
static void (*vt_test_force_raw_fn)(bool) = NULL;
static void (*vt_encode_timing_reset_fn)(uint64_t) = NULL;
static size_t (*vt_encode_timing_samples_fn)(uint64_t *, uint64_t *, size_t) = NULL;
static bool (*vt_encode_cpu_window_fn)(uint64_t *, uint64_t *, uint64_t *, uint64_t *) = NULL;
static void (*vt_encode_timing_watch_service_fn)(int) = NULL;
static bool (*vt_encode_service_cpu_window_fn)(uint64_t *) = NULL;

static bool resolve_symbol(const char *name, void *function_pointer, size_t size)
{
	(void)dlerror();
	void *symbol = dlsym(vt_plugin_handle, name);
	const char *error = dlerror();
	if (error || !symbol || sizeof(symbol) != size) {
		fprintf(stderr, "harness: cannot resolve %s: %s\n", name, error ? error : "missing symbol");
		return false;
	}
	memcpy(function_pointer, &symbol, size);
	return true;
}

static bool resolve_copy_counter(void)
{
	vt_plugin_handle = dlopen(vt_plugin_binary, RTLD_LAZY | RTLD_NOLOAD);
	if (!vt_plugin_handle) {
		const char *error = dlerror();
		fprintf(stderr, "harness: cannot find loaded VideoToolbox plugin at %s: %s\n", vt_plugin_binary,
			error ? error : "dlopen failed");
		return false;
	}

	(void)dlerror();
	void *reset_symbol = dlsym(vt_plugin_handle, "vt_copy_bytes_reset");
	const char *error = dlerror();
	if (error || !reset_symbol || sizeof(reset_symbol) != sizeof(vt_copy_bytes_reset_fn)) {
		fprintf(stderr, "harness: cannot resolve vt_copy_bytes_reset: %s\n", error ? error : "missing symbol");
		return false;
	}
	memcpy(&vt_copy_bytes_reset_fn, &reset_symbol, sizeof(vt_copy_bytes_reset_fn));

	(void)dlerror();
	void *total_symbol = dlsym(vt_plugin_handle, "vt_copy_bytes_total");
	error = dlerror();
	if (error || !total_symbol || sizeof(total_symbol) != sizeof(vt_copy_bytes_total_fn)) {
		fprintf(stderr, "harness: cannot resolve vt_copy_bytes_total: %s\n", error ? error : "missing symbol");
		return false;
	}
	memcpy(&vt_copy_bytes_total_fn, &total_symbol, sizeof(vt_copy_bytes_total_fn));

	(void)dlerror();
	void *submission_reset_symbol = dlsym(vt_plugin_handle, "vt_encode_submission_reset");
	error = dlerror();
	if (error || !submission_reset_symbol ||
	    sizeof(submission_reset_symbol) != sizeof(vt_encode_submission_reset_fn)) {
		fprintf(stderr, "harness: cannot resolve vt_encode_submission_reset: %s\n",
			error ? error : "missing symbol");
		return false;
	}
	memcpy(&vt_encode_submission_reset_fn, &submission_reset_symbol, sizeof(vt_encode_submission_reset_fn));

	(void)dlerror();
	void *submission_total_symbol = dlsym(vt_plugin_handle, "vt_encode_submission_total");
	error = dlerror();
	if (error || !submission_total_symbol ||
	    sizeof(submission_total_symbol) != sizeof(vt_encode_submission_total_fn)) {
		fprintf(stderr, "harness: cannot resolve vt_encode_submission_total: %s\n",
			error ? error : "missing symbol");
		return false;
	}
	memcpy(&vt_encode_submission_total_fn, &submission_total_symbol, sizeof(vt_encode_submission_total_fn));

	(void)dlerror();
	void *force_raw_symbol = dlsym(vt_plugin_handle, "vt_test_force_raw");
	error = dlerror();
	if (error || !force_raw_symbol || sizeof(force_raw_symbol) != sizeof(vt_test_force_raw_fn)) {
		fprintf(stderr, "harness: cannot resolve vt_test_force_raw: %s\n", error ? error : "missing symbol");
		return false;
	}
	memcpy(&vt_test_force_raw_fn, &force_raw_symbol, sizeof(vt_test_force_raw_fn));
	return resolve_symbol("vt_encode_timing_reset", &vt_encode_timing_reset_fn,
			      sizeof(vt_encode_timing_reset_fn)) &&
	       resolve_symbol("vt_encode_timing_samples", &vt_encode_timing_samples_fn,
			      sizeof(vt_encode_timing_samples_fn)) &&
	       resolve_symbol("vt_encode_cpu_window", &vt_encode_cpu_window_fn, sizeof(vt_encode_cpu_window_fn)) &&
	       resolve_symbol("vt_encode_timing_watch_service", &vt_encode_timing_watch_service_fn,
			      sizeof(vt_encode_timing_watch_service_fn)) &&
	       resolve_symbol("vt_encode_service_cpu_window", &vt_encode_service_cpu_window_fn,
			      sizeof(vt_encode_service_cpu_window_fn));
}

/* The VTEncoderXPCService instance VideoToolbox started for this process: the
 * newest one that started after `since_us`. Returns 0 when there is none and
 * counts every candidate, so a report shows when another process started one
 * at the same time. */
static pid_t find_vt_service(uint64_t since_us, int *candidates)
{
	*candidates = 0;
	int bytes = proc_listallpids(NULL, 0);
	if (bytes <= 0)
		return 0;
	size_t capacity = (size_t)bytes + 64;
	pid_t *pids = calloc(capacity, sizeof(pid_t));
	if (!pids)
		return 0;
	int count = proc_listallpids(pids, (int)(capacity * sizeof(pid_t)));
	pid_t found = 0;
	uint64_t found_start = 0;
	for (int i = 0; i < count; i++) {
		struct proc_bsdinfo info;
		if (proc_pidinfo(pids[i], PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != (int)sizeof(info))
			continue;
		if (strncmp(info.pbi_name, "VTEncoderXPCService", sizeof(info.pbi_name)) != 0)
			continue;
		uint64_t start = info.pbi_start_tvsec * 1000000ULL + info.pbi_start_tvusec;
		if (start < since_us)
			continue;
		(*candidates)++;
		if (start >= found_start) {
			found = pids[i];
			found_start = start;
		}
	}
	free(pids);
	return found;
}

static int compare_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

/* Nearest-rank percentile of a sorted array, in microseconds. */
static double percentile_us(const uint64_t *sorted, size_t count, double percent)
{
	if (!count)
		return 0.0;
	size_t rank = (size_t)(percent / 100.0 * (double)count + 0.999999);
	if (rank < 1)
		rank = 1;
	if (rank > count)
		rank = count;
	return (double)sorted[rank - 1] / 1000.0;
}

/* Appends to the timing line, which is written with one call so that libobs
 * log lines from other threads cannot split it. */
static __attribute__((format(printf, 3, 4))) void append(char *line, size_t size, const char *format, ...)
{
	size_t used = strlen(line);
	if (used >= size)
		return;
	va_list args;
	va_start(args, format);
	vsnprintf(line + used, size - used, format, args);
	va_end(args);
}

static void append_latency(char *line, size_t size, const char *name, uint64_t *values, size_t count)
{
	qsort(values, count, sizeof(*values), compare_u64);
	append(line, size, " %s_p50_us=%.1f %s_p95_us=%.1f %s_min_us=%.1f %s_max_us=%.1f", name,
	       percentile_us(values, count, 50), name, percentile_us(values, count, 95), name,
	       count ? (double)values[0] / 1000.0 : 0.0, name, count ? (double)values[count - 1] / 1000.0 : 0.0);
}

static uint64_t read_copy_bytes(void)
{
	return vt_copy_bytes_total_fn();
}

struct child_ctx {
	struct wait_state *state;
};

static bool collect_child(void *context, profiler_snapshot_entry_t *entry);

static void find_in_children(profiler_snapshot_entry_t *entry, struct wait_state *state)
{
	const char *name = profiler_snapshot_entry_name(entry);
	uint64_t count = profiler_snapshot_entry_overall_count(entry);
	if (name && strcmp(name, "download_frame") == 0)
		state->waits += count;
	struct child_ctx ctx = {state};
	profiler_snapshot_enumerate_children(entry, collect_child, &ctx);
}

static bool collect_child(void *context, profiler_snapshot_entry_t *entry)
{
	struct child_ctx *ctx = context;
	find_in_children(entry, ctx->state);
	return true;
}

static bool find_download_waits(void *context, profiler_snapshot_entry_t *entry)
{
	struct wait_state *state = context;
	find_in_children(entry, state);
	return true;
}

static uint64_t snapshot_download_waits(void)
{
	struct wait_state state = {0};
	profiler_snapshot_t *snap = profile_snapshot_create();
	if (!snap)
		return 0;
	profiler_snapshot_enumerate_roots(snap, find_download_waits, &state);
	profile_snapshot_free(snap);
	return state.waits;
}

/* The H.264 encoder OBS users get on Apple Silicon is VideoToolbox's hardware
 * one, com.apple.videotoolbox.videoencoder.ave.avc ("Apple VT H264 Hardware
 * Encoder"). com.apple.videotoolbox.videoencoder.h264 is the software encoder,
 * which the first version of this harness picked by matching "264".
 * --encoder=ID picks any registered encoder instead. */
#define VT_HARDWARE_H264 "com.apple.videotoolbox.videoencoder.ave.avc"

static const char *find_vt_encoder_id(const char *wanted)
{
	size_t idx = 0;
	const char *candidate = NULL;
	while (obs_enum_encoder_types(idx++, &candidate)) {
		if (candidate && strcmp(candidate, wanted ? wanted : VT_HARDWARE_H264) == 0)
			return candidate;
	}
	return NULL;
}

static bool load_build_tree_modules(const char *argv0)
{
	/* The harness binary lives at <build>/test/perf/<Config>/. Mirror
	 * the frontend's macOS loader: one %module% pattern covering every
	 * dev-tree plugin bundle underneath <build>/plugins. Returns false
	 * if any module fails. */
	char exe[1024];
	char bin[1152];
	char data[1152];

	if (!realpath(argv0, exe))
		return false;
	char *slash = strrchr(exe, '/');
	if (!slash)
		return false;
	*slash = 0; /* executable name */
	slash = strrchr(exe, '/');
	if (!slash)
		return false;
	char config[64];
	snprintf(config, sizeof(config), "%s", slash + 1);
	*slash = 0; /* Debug, RelWithDebInfo, etc. */
	for (int i = 0; i < 2; i++) {
		slash = strrchr(exe, '/');
		if (!slash)
			return false;
		*slash = 0; /* perf, test */
	}
	snprintf(bin, sizeof(bin), "%s/plugins/%%module%%/%s/%%module%%.plugin/Contents/MacOS", exe, config);
	snprintf(data, sizeof(data), "%s/plugins/%%module%%/%s/%%module%%.plugin/Contents/Resources", exe, config);
	snprintf(vt_plugin_binary, sizeof(vt_plugin_binary),
		 "%s/plugins/mac-videotoolbox/%s/mac-videotoolbox.plugin/Contents/MacOS/mac-videotoolbox", exe, config);

	struct obs_module_failure_info error = {0};
	struct obs_runtime_module_info info = {
		.path_info = {.binary = bin, .data = data},
		/* PLUGIN, not LEGACY_PLUGIN: these names are on the core-module
		 * list, which legacy entries skip. */
		.type = OBS_MODULE_TYPE_PLUGIN,
		.name = NULL,
	};
	obs_load_plugins(&info, &error);
	bool ok = error.count == 0;
	if (!ok)
		fprintf(stderr, "harness: %u module(s) failed to load\n", (unsigned)error.count);
	obs_module_failure_info_free(&error);

	/* Runs obs_module_post_load hooks (e.g. mac-videotoolbox registers
	 * its encoders there, after its async encoder list resolves). */
	obs_post_load_modules();
	return ok;
}

int main(int argc, char **argv)
{
	bool force_raw_fallback = false;
	bool raw_planar = false;
	bool raw_no_gpu = false;
	int want_frames = DEFAULT_FRAMES;
	long warmup = -1;
	const char *wanted_encoder = NULL;
	struct timeval harness_start;
	gettimeofday(&harness_start, NULL);
	uint64_t harness_start_us = (uint64_t)harness_start.tv_sec * 1000000ULL + (uint64_t)harness_start.tv_usec;
	for (int i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--encoder=", 10) == 0) {
			wanted_encoder = argv[i] + 10;
		} else if (strncmp(argv[i], "--warmup=", 9) == 0) {
			char *end = NULL;
			warmup = strtol(argv[i] + 9, &end, 10);
			if (!argv[i][9] || !end || *end || warmup <= 0) {
				fprintf(stderr, "harness: --warmup needs a positive frame count\n");
				return 1;
			}
		} else if (strcmp(argv[i], "--raw-fallback") == 0) {
			force_raw_fallback = true;
		} else if (strcmp(argv[i], "--raw-planar") == 0) {
			raw_planar = true;
		} else if (strcmp(argv[i], "--raw-no-gpu") == 0 || strcmp(argv[i], "--raw") == 0) {
			raw_no_gpu = true;
		} else {
			char *end = NULL;
			long parsed = strtol(argv[i], &end, 10);
			if (!argv[i][0] || !end || *end || parsed <= 0 || parsed > INT32_MAX) {
				fprintf(stderr,
					"usage: %s [frames=%d] [--warmup=N] [--encoder=ID] [--raw-fallback|--raw-planar|--raw-no-gpu]\n",
					argv[0], DEFAULT_FRAMES);
				return 1;
			}
			want_frames = (int)parsed;
		}
	}
	if (warmup < 0)
		warmup = want_frames / 5 > 0 ? want_frames / 5 : 1;
	if (warmup >= want_frames) {
		fprintf(stderr, "harness: --warmup must be below the frame count\n");
		return 1;
	}
	if ((int)force_raw_fallback + (int)raw_planar + (int)raw_no_gpu > 1) {
		fprintf(stderr, "harness: choose one raw mode\n");
		return 1;
	}
	/* Read once by libobs-metal, before the renderer is created. */
	if (raw_planar)
		setenv("OBS_METAL_DISABLE_NV12", "1", 1);

	if (!obs_startup("en", NULL, NULL)) {
		fprintf(stderr, "harness: obs_startup failed\n");
		return 1;
	}

	/* Before video reset: threads latch the disabled state permanently
	 * otherwise (only lock_root failures re-arm it, and the render
	 * thread never reenables itself). */
	profiler_start();

	struct obs_video_info ovi = {0};
	ovi.adapter = 0;
	ovi.fps_num = 60;
	ovi.fps_den = 1;
	/* Bare filename with extension: os_dlopen only appends ".so" when no
	 * known extension is present, so this resolves via DYLD_LIBRARY_PATH
	 * (which must include the built libobs-metal directory). */
	ovi.graphics_module = "libobs-metal.dylib";
	/* GPU conversion path: the M1 zero-copy work happens here (NV12
	 * textures staged on the GPU). Raw path would bypass it. */
	ovi.gpu_conversion = !raw_no_gpu;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.base_width = 1920;
	ovi.base_height = 1080;
	ovi.output_width = 1920;
	ovi.output_height = 1080;

	if (obs_reset_video(&ovi) != 0) {
		fprintf(stderr, "harness: obs_reset_video (Metal) failed\n");
		obs_shutdown();
		return 1;
	}

	struct obs_audio_info oai = {0};
	oai.samples_per_sec = 48000;
	oai.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio(&oai)) {
		fprintf(stderr, "harness: obs_reset_audio failed\n");
		obs_shutdown();
		return 1;
	}

	if (!load_build_tree_modules(argv[0])) {
		fprintf(stderr, "harness: module load reported failures\n");
		obs_shutdown();
		return 1;
	}
	if (!resolve_copy_counter()) {
		if (vt_plugin_handle)
			dlclose(vt_plugin_handle);
		obs_shutdown();
		return 1;
	}
	vt_test_force_raw_fn(force_raw_fallback || raw_planar || raw_no_gpu);

	obs_scene_t *scene = obs_scene_create("harness scene");
	obs_source_t *color = obs_source_create("color_source", "harness color", NULL, NULL);
	if (!scene || !color) {
		fprintf(stderr, "harness: scene/source setup failed\n");
		obs_shutdown();
		return 1;
	}
	obs_scene_add(scene, color);
	obs_set_output_source(0, obs_scene_get_source(scene));

	const char *enc_id = find_vt_encoder_id(wanted_encoder);
	if (!enc_id) {
		fprintf(stderr, "harness: VideoToolbox encoder %s is not registered\n",
			wanted_encoder ? wanted_encoder : VT_HARDWARE_H264);
		obs_shutdown();
		return 1;
	}

	obs_data_t *enc_settings = obs_data_create();
	obs_data_set_int(enc_settings, "bitrate", 2500);
	obs_encoder_t *encoder = obs_video_encoder_create(enc_id, "harness vt", enc_settings, NULL);
	obs_data_release(enc_settings);
	if (!encoder) {
		fprintf(stderr, "harness: encoder create failed\n");
		obs_shutdown();
		return 1;
	}

	obs_output_t *output = obs_output_create("null_output", "harness null", NULL, NULL);
	if (!output) {
		fprintf(stderr, "harness: null output create failed\n");
		obs_shutdown();
		return 1;
	}
	obs_encoder_set_video(encoder, obs_get_video());
	obs_output_set_video_encoder(output, encoder);

	/* Null output carries audio too; Core Audio AAC is Apple-native. */
	obs_data_t *aenc_settings = obs_data_create();
	obs_data_set_int(aenc_settings, "bitrate", 160);
	obs_encoder_t *audio_encoder = obs_audio_encoder_create("CoreAudio_AAC", "harness aac", aenc_settings, 0, NULL);
	obs_data_release(aenc_settings);
	if (audio_encoder) {
		obs_encoder_set_audio(audio_encoder, obs_get_audio());
		obs_output_set_audio_encoder(output, audio_encoder, 0);
	}
	vt_copy_bytes_reset_fn();
	vt_encode_submission_reset_fn((uint64_t)want_frames);
	vt_encode_timing_reset_fn((uint64_t)warmup);
	const int frames_before = obs_output_get_total_frames(output);
	const uint64_t waits_before = snapshot_download_waits();
	const uint32_t render_total_before = obs_get_total_frames();
	const uint32_t render_lagged_before = obs_get_lagged_frames();
	if (!obs_output_start(output)) {
		fprintf(stderr, "harness: output start failed: %s\n", obs_output_get_last_error(output));
		dlclose(vt_plugin_handle);
		obs_shutdown();
		return 1;
	}

	int waited_ms = 0;
	pid_t service_pid = 0;
	int service_candidates = 0;
	while (vt_encode_submission_total_fn() < (uint64_t)want_frames && waited_ms < TIMEOUT_MS &&
	       obs_output_active(output)) {
		/* The session (and its service) exists once a frame was accepted;
		 * the window opens `warmup` frames later. */
		if (!service_pid && vt_encode_submission_total_fn() > 0) {
			service_pid = find_vt_service(harness_start_us, &service_candidates);
			if (service_pid)
				vt_encode_timing_watch_service_fn((int)service_pid);
		}
		os_sleep_ms(50);
		waited_ms += 50;
	}
	/* Before the stop: video-io's counters start when the encoder connects
	 * and are only logged, not kept, when it disconnects. A texture
	 * encoder's skipped frames are frames it was given again because no new
	 * texture was free (or rendering lagged); a raw encoder's are frames
	 * video-io dropped because the encoder lagged. */
	uint32_t video_total = 0, video_skipped = 0;
	obs_get_video_frame_counts(&video_total, &video_skipped);
	uint32_t render_total = obs_get_total_frames() - render_total_before;
	uint32_t render_lagged = obs_get_lagged_frames() - render_lagged_before;
	obs_output_stop(output);
	int frames = obs_output_get_total_frames(output) - frames_before;
	uint64_t submissions = vt_encode_submission_total_fn();
	uint64_t waits_after = snapshot_download_waits();
	uint64_t copied_bytes = read_copy_bytes();
	uint64_t waits = waits_after >= waits_before ? waits_after - waits_before : 0;
	uint64_t bytes_per_submission = submissions ? copied_bytes / submissions : 0;
	uint64_t bytes_remainder = submissions ? copied_bytes % submissions : copied_bytes;
	const char *path = force_raw_fallback ? "forced-raw-fallback"
			   : raw_planar       ? "raw-planar"
			   : raw_no_gpu       ? "raw-no-gpu"
					      : "texture";
	printf("resolution=1920x1080 format=NV12 gpu_conversion=%s path=%s submissions=%llu output_frames=%d "
	       "copied_bytes=%llu copied_bytes_per_submission=%llu remainder=%llu waits=%llu\n",
	       raw_no_gpu ? "false" : "true", path, (unsigned long long)submissions, frames,
	       (unsigned long long)copied_bytes, (unsigned long long)bytes_per_submission,
	       (unsigned long long)bytes_remainder, (unsigned long long)waits);

	uint64_t cpu_frames = 0, cpu_wall_ns = 0, cpu_user_ns = 0, cpu_system_ns = 0, service_ns = 0;
	bool cpu_window = vt_encode_cpu_window_fn(&cpu_frames, &cpu_wall_ns, &cpu_user_ns, &cpu_system_ns);
	bool service_window = vt_encode_service_cpu_window_fn(&service_ns);
	uint64_t *entry_latency = calloc((size_t)want_frames, sizeof(uint64_t));
	uint64_t *submit_latency = calloc((size_t)want_frames, sizeof(uint64_t));
	size_t latency_count = 0;
	if (entry_latency && submit_latency)
		latency_count = vt_encode_timing_samples_fn(entry_latency, submit_latency, (size_t)want_frames);
	size_t skipped = latency_count < (size_t)warmup ? latency_count : (size_t)warmup;
	size_t measured = latency_count - skipped;
	char line[2048] = "";
	append(line, sizeof(line),
	       "timing path=%s warmup=%ld cpu_window=%s cpu_frames=%llu cpu_wall_ms=%.3f cpu_user_ms=%.3f "
	       "cpu_system_ms=%.3f cpu_us_per_frame=%.2f cpu_percent=%.2f render_frames=%u render_lagged=%u "
	       "video_frames=%u video_skipped=%u encoder=%s service_pid=%d service_candidates=%d service_window=%s "
	       "service_cpu_ms=%.3f service_us_per_frame=%.2f total_us_per_frame=%.2f latency_samples=%zu",
	       path, warmup, cpu_window ? "true" : "false", (unsigned long long)cpu_frames, (double)cpu_wall_ns / 1e6,
	       (double)cpu_user_ns / 1e6, (double)cpu_system_ns / 1e6,
	       cpu_frames ? (double)(cpu_user_ns + cpu_system_ns) / 1000.0 / (double)cpu_frames : 0.0,
	       cpu_wall_ns ? 100.0 * (double)(cpu_user_ns + cpu_system_ns) / (double)cpu_wall_ns : 0.0, render_total,
	       render_lagged, video_total, video_skipped, enc_id, (int)service_pid, service_candidates,
	       service_window ? "true" : "false", (double)service_ns / 1e6,
	       cpu_frames ? (double)service_ns / 1000.0 / (double)cpu_frames : 0.0,
	       cpu_frames ? (double)(cpu_user_ns + cpu_system_ns + service_ns) / 1000.0 / (double)cpu_frames : 0.0,
	       measured);
	if (entry_latency && submit_latency) {
		append_latency(line, sizeof(line), "entry_to_output", entry_latency + skipped, measured);
		append_latency(line, sizeof(line), "submit_to_output", submit_latency + skipped, measured);
	}
	append(line, sizeof(line), "\n");
	fflush(stdout);
	fputs(line, stdout);
	fflush(stdout);
	free(entry_latency);
	free(submit_latency);

	obs_output_release(output);
	obs_encoder_release(encoder);
	if (audio_encoder)
		obs_encoder_release(audio_encoder);
	obs_set_output_source(0, NULL);
	obs_source_remove(obs_scene_get_source(scene));
	obs_scene_release(scene);
	obs_source_release(color);
	obs_shutdown();
	dlclose(vt_plugin_handle);

	if (!cpu_window || cpu_frames != (uint64_t)(want_frames - warmup)) {
		fprintf(stderr, "harness: CPU window did not cover submissions %ld to %d\n", warmup, want_frames);
		return 1;
	}
	if (!service_window || service_candidates != 1) {
		fprintf(stderr, "harness: VTEncoderXPCService CPU not read (pid %d, %d candidates)\n", (int)service_pid,
			service_candidates);
		return 1;
	}
	if (submissions != (uint64_t)want_frames) {
		fprintf(stderr, "harness: expected %d accepted VideoToolbox submissions, got %llu\n", want_frames,
			(unsigned long long)submissions);
		return 1;
	}
	if (force_raw_fallback || raw_planar || raw_no_gpu) {
		uint64_t expected_copy_bytes = submissions * NV12_1080P_BYTES;
		if (copied_bytes != expected_copy_bytes) {
			fprintf(stderr,
				"harness: raw path copied %llu bytes for %llu accepted submissions; expected %llu\n",
				(unsigned long long)copied_bytes, (unsigned long long)submissions,
				(unsigned long long)expected_copy_bytes);
			return 1;
		}
	} else if (copied_bytes != 0 || waits != 0) {
		fprintf(stderr, "harness: texture path copied %llu bytes and recorded %llu download_frame waits\n",
			(unsigned long long)copied_bytes, (unsigned long long)waits);
		return 1;
	}
	return 0;
}
