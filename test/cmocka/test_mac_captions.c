#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <cmocka.h>

#include <obs-internal.h>
#include <caption/eia608.h>

#include "../../plugins/mac-captions/captions-filter.h"
#include "../../plugins/mac-captions/speech-transcriber.h"

#define TEST_SAMPLE_RATE 48000
#define TEST_FRAMES 480

struct fake_word_event {
	double at_sec;
	char text[32];
};

struct fake_transcriber {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	void (*word)(void *param, const char *text, double start_sec, double end_sec);
	void *word_param;
	uint64_t frames_seen;
	unsigned starts;
	unsigned stops;
	struct fake_word_event words[8];
	size_t word_count;
	size_t next_word;
	bool fixture_ok;
	bool sample_rate_ok;
	bool running;
	unsigned emitted_words;
};

struct fake_output {
	struct obs_output output;
	struct caption_track_data track;
};

static void *fake_create(void *factory_data)
{
	return factory_data;
}

static bool fake_start(void *session, uint32_t sample_rate, void (*word)(void *, const char *, double, double),
		       void *param)
{
	struct fake_transcriber *fake = session;
	fake->sample_rate_ok = sample_rate == TEST_SAMPLE_RATE;
	FILE *fixture = fopen(MAC_CAPTIONS_WORDS_FIXTURE, "r");
	if (!fixture)
		return false;
	while (fake->word_count < sizeof(fake->words) / sizeof(fake->words[0]) &&
	       fscanf(fixture, "%lf %31s", &fake->words[fake->word_count].at_sec, fake->words[fake->word_count].text) ==
		       2)
		++fake->word_count;
	fake->fixture_ok = !ferror(fixture) && fake->word_count == 3;
	fclose(fixture);
	bool ok = fake->sample_rate_ok && fake->fixture_ok;
	pthread_mutex_lock(&fake->mutex);
	if (ok) {
		fake->word = word;
		fake->word_param = param;
		fake->running = true;
		++fake->starts;
	}
	pthread_cond_broadcast(&fake->cond);
	pthread_mutex_unlock(&fake->mutex);
	return ok;
}

static bool fake_push(void *session, const float *mono, uint32_t frames, uint64_t timestamp_ns)
{
	struct fake_transcriber *fake = session;
	pthread_mutex_lock(&fake->mutex);
	if (!mono || !frames)
		fake->fixture_ok = false;
	fake->frames_seen += frames;
	double end = (double)timestamp_ns / 1e9 + (double)frames / TEST_SAMPLE_RATE;
	struct fake_word_event due[8];
	size_t due_count = 0;
	while (fake->next_word < fake->word_count && fake->words[fake->next_word].at_sec <= end) {
		due[due_count++] = fake->words[fake->next_word++];
	}
	void (*word)(void *, const char *, double, double) = fake->word;
	void *param = fake->word_param;
	pthread_cond_broadcast(&fake->cond);
	pthread_mutex_unlock(&fake->mutex);

	for (size_t i = 0; word && i < due_count; ++i) {
		word(param, due[i].text, due[i].at_sec, due[i].at_sec + 0.12);
		pthread_mutex_lock(&fake->mutex);
		++fake->emitted_words;
		pthread_cond_broadcast(&fake->cond);
		pthread_mutex_unlock(&fake->mutex);
	}
	return true;
}

static void fake_stop(void *session)
{
	struct fake_transcriber *fake = session;
	pthread_mutex_lock(&fake->mutex);
	fake->running = false;
	++fake->stops;
	pthread_cond_broadcast(&fake->cond);
	pthread_mutex_unlock(&fake->mutex);
}

static void fake_destroy(void *session)
{
	(void)session;
}

static const struct mac_captions_transcriber_ops fake_ops = {
	.create = fake_create,
	.start = fake_start,
	.push = fake_push,
	.stop = fake_stop,
	.destroy = fake_destroy,
};

static void wait_for_counter(struct fake_transcriber *fake, unsigned *value, unsigned target)
{
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 3;
	pthread_mutex_lock(&fake->mutex);
	while (*value < target) {
		int rc = pthread_cond_timedwait(&fake->cond, &fake->mutex, &deadline);
		if (rc != 0)
			break;
	}
	assert_true(*value >= target);
	pthread_mutex_unlock(&fake->mutex);
}

static void wait_for_frames(struct fake_transcriber *fake, uint64_t target)
{
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 3;
	pthread_mutex_lock(&fake->mutex);
	while (fake->frames_seen < target) {
		int rc = pthread_cond_timedwait(&fake->cond, &fake->mutex, &deadline);
		if (rc != 0)
			break;
	}
	assert_true(fake->frames_seen >= target);
	pthread_mutex_unlock(&fake->mutex);
}

static FILE *open_fixture(const char *path)
{
	FILE *file = fopen(path, "rb");
	assert_non_null(file);
	uint8_t header[12];
	assert_int_equal(fread(header, 1, sizeof(header), file), sizeof(header));
	assert_memory_equal(header, "RIFF", 4);
	assert_memory_equal(header + 8, "WAVE", 4);
	bool have_format = false;
	while (true) {
		uint8_t chunk[8];
		assert_int_equal(fread(chunk, 1, sizeof(chunk), file), sizeof(chunk));
		uint32_t size = (uint32_t)chunk[4] | ((uint32_t)chunk[5] << 8) | ((uint32_t)chunk[6] << 16) |
				((uint32_t)chunk[7] << 24);
		if (memcmp(chunk, "data", 4) == 0) {
			assert_true(have_format);
			break;
		}
		if (memcmp(chunk, "fmt ", 4) == 0) {
			uint8_t format[16];
			assert_true(size >= sizeof(format));
			assert_int_equal(fread(format, 1, sizeof(format), file), sizeof(format));
			assert_int_equal(format[0] | (format[1] << 8), 1);
			assert_int_equal(format[2] | (format[3] << 8), 1);
			assert_int_equal((uint32_t)format[4] | ((uint32_t)format[5] << 8) |
						 ((uint32_t)format[6] << 16) | ((uint32_t)format[7] << 24),
					 TEST_SAMPLE_RATE);
			assert_int_equal(format[14] | (format[15] << 8), 16);
			assert_true(fseek(file, (long)(size - sizeof(format)), SEEK_CUR) == 0);
			have_format = true;
		} else {
			assert_true(fseek(file, (long)size, SEEK_CUR) == 0);
		}
		if (size & 1)
			assert_true(fseek(file, 1, SEEK_CUR) == 0);
	}
	return file;
}

static size_t feed_fixture(struct mac_captions_core *core, const char *path)
{
	FILE *file = open_fixture(path);
	int16_t pcm[TEST_FRAMES];
	float samples[TEST_FRAMES];
	size_t frames_total = 0;
	while (true) {
		size_t frames = fread(pcm, sizeof(*pcm), TEST_FRAMES, file);
		if (!frames)
			break;
		for (size_t i = 0; i < frames; ++i)
			samples[i] = (float)pcm[i] / 32768.0f;
		struct obs_audio_data audio = {0};
		audio.frames = (uint32_t)frames;
		audio.timestamp = (uint64_t)frames_total * 1000000000ULL / TEST_SAMPLE_RATE;
		audio.data[0] = (uint8_t *)samples;
		assert_ptr_equal(mac_captions_core_filter_audio(core, &audio), &audio);
		frames_total += frames;
	}
	assert_false(ferror(file));
	fclose(file);
	return frames_total;
}

static void deliver_caption_to_output(void *param, const uint8_t *data)
{
	if (!obs_output_active(param))
		return;
	struct obs_source_cea_708 captions = {.data = data, .packets = 1};
	obs_output_caption(param, &captions);
}

static void init_fake(struct fake_transcriber *fake)
{
	memset(fake, 0, sizeof(*fake));
	assert_int_equal(pthread_mutex_init(&fake->mutex, NULL), 0);
	assert_int_equal(pthread_cond_init(&fake->cond, NULL), 0);
}

static void destroy_fake(struct fake_transcriber *fake)
{
	pthread_cond_destroy(&fake->cond);
	pthread_mutex_destroy(&fake->mutex);
}

static void test_file_audio_tap_enable_disable_rollup_and_output(void **state)
{
	(void)state;
	struct fake_transcriber fake;
	init_fake(&fake);
	struct mac_captions_core *core = mac_captions_core_create(&fake_ops, &fake, NULL, TEST_SAMPLE_RATE, 1, false);
	assert_non_null(core);

	assert_true(feed_fixture(core, MAC_CAPTIONS_AUDIO_FIXTURE) >= TEST_SAMPLE_RATE);
	assert_int_equal(fake.starts, 0);
	assert_false(mac_captions_core_is_running(core));

	mac_captions_core_set_enabled(core, true);
	wait_for_counter(&fake, &fake.starts, 1);
	assert_true(fake.sample_rate_ok);
	assert_true(fake.fixture_ok);
	assert_true(feed_fixture(core, MAC_CAPTIONS_AUDIO_FIXTURE) >= TEST_SAMPLE_RATE);
	wait_for_frames(&fake, TEST_SAMPLE_RATE);
	assert_true(fake.fixture_ok);
	assert_true(fake.running);
	assert_true(mac_captions_core_is_running(core));
	wait_for_counter(&fake, &fake.emitted_words, 3);

	struct fake_output output = {0};
	pthread_mutex_init(&output.track.caption_mutex, NULL);
	deque_init(&output.track.caption_data);
	output.output.caption_tracks[0] = &output.track;
	os_atomic_set_bool(&output.output.active, true);
	unsigned ticks = 0;
	while (mac_captions_core_drain_one(core, deliver_caption_to_output, &output.output)) {
		assert_int_equal(output.track.caption_data.size, (size_t)(++ticks) * 3);
		assert_true(ticks < 128);
	}
	assert_true(ticks > 12);

	uint8_t triplet[3];
	deque_pop_front(&output.track.caption_data, triplet, sizeof(triplet));
	assert_int_equal(triplet[0], 0xFC);
	uint16_t pair = (uint16_t)((triplet[1] << 8) | triplet[2]);
	assert_int_equal(pair, eia608_control_command(eia608_control_erase_display_memory, 0));
	char hello[] = "hello";
	char from[] = "from";
	char fixture[] = "fixture";
	uint16_t hello_pair = eia608_from_utf8_2(&hello[0], &hello[1]);
	uint16_t from_pair = eia608_from_utf8_2(&from[0], &from[1]);
	uint16_t fixture_pair = eia608_from_utf8_2(&fixture[0], &fixture[1]);
	bool saw_hello = false;
	bool saw_from = false;
	bool saw_fixture = false;
	while (output.track.caption_data.size >= sizeof(triplet)) {
		deque_pop_front(&output.track.caption_data, triplet, sizeof(triplet));
		pair = (uint16_t)((triplet[1] << 8) | triplet[2]);
		saw_hello |= pair == hello_pair;
		saw_from |= pair == from_pair;
		saw_fixture |= pair == fixture_pair;
	}
	assert_true(saw_hello);
	assert_true(saw_from);
	assert_true(saw_fixture);

	mac_captions_core_set_enabled(core, false);
	wait_for_counter(&fake, &fake.stops, 1);
	assert_false(fake.running);
	assert_false(mac_captions_core_is_running(core));
	assert_false(mac_captions_core_drain_one(core, deliver_caption_to_output, &output.output));
	assert_int_equal(fake.starts, 1);

	os_atomic_set_bool(&output.output.active, false);
	deque_free(&output.track.caption_data);
	pthread_mutex_destroy(&output.track.caption_mutex);
	mac_captions_core_destroy(core);
	destroy_fake(&fake);
}

struct transcript_capture {
	char text[2048];
	size_t length;
};

static uint64_t monotonic_ns(void)
{
	struct timespec now;
	assert_int_equal(clock_gettime(CLOCK_MONOTONIC, &now), 0);
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static void sleep_until_ns(uint64_t deadline)
{
	while (true) {
		uint64_t now = monotonic_ns();
		if (now >= deadline)
			return;
		uint64_t remaining = deadline - now;
		struct timespec duration = {
			.tv_sec = (time_t)(remaining / 1000000000ULL),
			.tv_nsec = (long)(remaining % 1000000000ULL),
		};
		nanosleep(&duration, NULL);
	}
}

static void drain_output_at_video_rate(struct mac_captions_core *core, struct fake_output *output,
				       uint64_t *next_tick_ns)
{
	uint64_t now = monotonic_ns();
	while (now >= *next_tick_ns) {
		mac_captions_core_drain_one(core, deliver_caption_to_output, &output->output);
		*next_tick_ns += 16666667ULL;
		now = monotonic_ns();
	}
}

static size_t feed_speech_fixture_through_filter(struct mac_captions_core *core, const char *path,
						 struct fake_output *output)
{
	FILE *file = open_fixture(path);
	int16_t pcm[TEST_FRAMES];
	float samples[TEST_FRAMES];
	size_t frames_total = 0;
	uint64_t next_audio_ns = monotonic_ns();
	uint64_t next_video_tick_ns = next_audio_ns;
	while (true) {
		size_t frames = fread(pcm, sizeof(*pcm), TEST_FRAMES, file);
		if (!frames)
			break;
		for (size_t i = 0; i < frames; ++i)
			samples[i] = (float)pcm[i] / 32768.0f;
		struct obs_audio_data audio = {0};
		audio.frames = (uint32_t)frames;
		audio.timestamp = (uint64_t)frames_total * 1000000000ULL / TEST_SAMPLE_RATE;
		audio.data[0] = (uint8_t *)samples;
		assert_ptr_equal(mac_captions_core_filter_audio(core, &audio), &audio);
		frames_total += frames;
		next_audio_ns += (uint64_t)frames * 1000000000ULL / TEST_SAMPLE_RATE;
		drain_output_at_video_rate(core, output, &next_video_tick_ns);
		sleep_until_ns(next_audio_ns);
	}
	assert_false(ferror(file));
	fclose(file);
	return frames_total;
}

static void feed_silence_through_filter(struct mac_captions_core *core, struct fake_output *output,
					uint64_t first_frame, size_t total_frames)
{
	float silence[TEST_FRAMES] = {0};
	uint64_t next_audio_ns = monotonic_ns();
	uint64_t next_video_tick_ns = next_audio_ns;
	for (size_t offset = 0; offset < total_frames; offset += TEST_FRAMES) {
		size_t frames = total_frames - offset;
		if (frames > TEST_FRAMES)
			frames = TEST_FRAMES;
		struct obs_audio_data audio = {0};
		audio.frames = (uint32_t)frames;
		audio.timestamp = (first_frame + offset) * 1000000000ULL / TEST_SAMPLE_RATE;
		audio.data[0] = (uint8_t *)silence;
		assert_ptr_equal(mac_captions_core_filter_audio(core, &audio), &audio);
		next_audio_ns += (uint64_t)frames * 1000000000ULL / TEST_SAMPLE_RATE;
		drain_output_at_video_rate(core, output, &next_video_tick_ns);
		sleep_until_ns(next_audio_ns);
	}
}

static void append_decoded_text(struct transcript_capture *capture, const char *text)
{
	for (const char *p = text; *p && capture->length + 1 < sizeof(capture->text); ++p)
		capture->text[capture->length++] = *p;
}

static void collect_decoded_608(struct fake_output *output, struct transcript_capture *capture, size_t *packets)
{
	uint8_t triplet[3];
	while (output->track.caption_data.size >= sizeof(triplet)) {
		deque_pop_front(&output->track.caption_data, triplet, sizeof(triplet));
		++*packets;
		if (triplet[0] != 0xFC)
			continue;

		uint16_t pair = (uint16_t)((triplet[1] << 8) | triplet[2]);
		int channel = 0;
		if (eia608_is_control(pair)) {
			if (eia608_parse_control(pair, &channel) == eia608_control_carriage_return && capture->length &&
			    capture->text[capture->length - 1] != ' ')
				append_decoded_text(capture, " ");
			continue;
		}
		char first[8] = {0};
		char second[8] = {0};
		int count = eia608_to_utf8(pair, &channel, first, second);
		if (count > 0)
			append_decoded_text(capture, first);
		if (count > 1)
			append_decoded_text(capture, second);
	}
	capture->text[capture->length] = '\0';
}

static size_t normalized_words(const char *text, char words[][64], size_t capacity)
{
	size_t count = 0;
	while (*text && count < capacity) {
		while (*text && !isalnum((unsigned char)*text))
			++text;
		size_t length = 0;
		while (*text && isalnum((unsigned char)*text)) {
			if (length + 1 < sizeof(words[count]))
				words[count][length++] = (char)tolower((unsigned char)*text);
			++text;
		}
		if (length) {
			words[count][length] = '\0';
			++count;
		}
	}
	return count;
}

static void test_real_speech_file_transcript(void **state)
{
	(void)state;
	int32_t status = mac_captions_speech_asset_status();
	if (status != MAC_CAPTIONS_ASSETS_INSTALLED) {
		print_message(
			"SKIP real speech through filter: en_US SpeechTranscriber asset is not installed (status=%d)\n",
			status);
		skip();
	}

	struct fake_output output = {0};
	assert_int_equal(pthread_mutex_init(&output.track.caption_mutex, NULL), 0);
	deque_init(&output.track.caption_data);
	output.output.caption_tracks[0] = &output.track;
	os_atomic_set_bool(&output.output.active, true);
	struct mac_captions_core *core =
		mac_captions_core_create(&mac_captions_speech_transcriber_ops, NULL, NULL, TEST_SAMPLE_RATE, 1, true);
	assert_non_null(core);

	uint64_t start_deadline = monotonic_ns() + 15000000000ULL;
	while (!mac_captions_core_is_running(core) && monotonic_ns() < start_deadline) {
		struct timespec interval = {.tv_sec = 0, .tv_nsec = 10000000L};
		nanosleep(&interval, NULL);
	}
	assert_true(mac_captions_core_is_running(core));
	size_t frames = feed_speech_fixture_through_filter(core, MAC_CAPTIONS_SPEECH_FIXTURE, &output);
	assert_int_equal(frames, (size_t)30 * TEST_SAMPLE_RATE);
	/* Trailing file-fed silence lets the recognizer finalize the last utterance
	 * without disabling the filter before its final words are delivered. */
	feed_silence_through_filter(core, &output, frames, 3 * TEST_SAMPLE_RATE);

	/* Keep the real-time filter enabled while SpeechTranscriber finalizes its
	 * last results; video ticks continue delivering the queued 608 roll-up. */
	uint64_t next_video_tick_ns = monotonic_ns();
	uint64_t finish_deadline = next_video_tick_ns + 5000000000ULL;
	while (monotonic_ns() < finish_deadline) {
		drain_output_at_video_rate(core, &output, &next_video_tick_ns);
		uint64_t wake = monotonic_ns() + 5000000ULL;
		if (wake > finish_deadline)
			wake = finish_deadline;
		sleep_until_ns(wake);
	}
	mac_captions_core_set_enabled(core, false);
	assert_false(mac_captions_core_is_running(core));
	assert_false(mac_captions_core_drain_one(core, deliver_caption_to_output, &output.output));

	FILE *reference_file = fopen(MAC_CAPTIONS_REFERENCE_FIXTURE, "r");
	assert_non_null(reference_file);
	char reference[4096] = {0};
	assert_non_null(fgets(reference, sizeof(reference), reference_file));
	fclose(reference_file);
	struct transcript_capture capture = {0};
	size_t packets = 0;
	collect_decoded_608(&output, &capture, &packets);
	assert_true(packets > 0);

	char hypothesis_words[128][64] = {{0}};
	char reference_words[128][64] = {{0}};
	size_t hypothesis_count = normalized_words(capture.text, hypothesis_words, 128);
	size_t reference_count = normalized_words(reference, reference_words, 128);
	assert_true(reference_count > 0);
	assert_true(hypothesis_count > 0);

	unsigned distance[129][129] = {{0}};
	for (size_t i = 0; i <= hypothesis_count; ++i)
		distance[i][0] = (unsigned)i;
	for (size_t j = 0; j <= reference_count; ++j)
		distance[0][j] = (unsigned)j;
	for (size_t i = 1; i <= hypothesis_count; ++i) {
		for (size_t j = 1; j <= reference_count; ++j) {
			unsigned substitution =
				distance[i - 1][j - 1] + (strcmp(hypothesis_words[i - 1], reference_words[j - 1]) != 0);
			unsigned deletion = distance[i - 1][j] + 1;
			unsigned insertion = distance[i][j - 1] + 1;
			distance[i][j] = substitution < deletion ? substitution : deletion;
			if (insertion < distance[i][j])
				distance[i][j] = insertion;
		}
	}
	double wer = (double)distance[hypothesis_count][reference_count] / (double)reference_count;
	print_message("real filter speech: %zu frames (%.1f s); 608 packets: %zu; transcript: %zu words; "
		      "reference: %zu words; WER %.1f%%\n",
		      frames, (double)frames / TEST_SAMPLE_RATE, packets, hypothesis_count, reference_count,
		      wer * 100.0);
	print_message("608 decoded output: %s\n", capture.text);
	assert_true(wer <= 0.50);

	os_atomic_set_bool(&output.output.active, false);
	deque_free(&output.track.caption_data);
	pthread_mutex_destroy(&output.track.caption_mutex);
	mac_captions_core_destroy(core);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_file_audio_tap_enable_disable_rollup_and_output),
		cmocka_unit_test(test_real_speech_file_transcript),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
