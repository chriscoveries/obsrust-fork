#include "captions-rollup.h"

#include <string.h>

#include <caption/eia608.h>

#define ROLLUP_COLUMNS 32
#define ROLLUP_BOTTOM_ROW 14

static bool push_locked(struct captions_rollup *rollup, uint16_t pair)
{
	if (rollup->count == sizeof(rollup->pairs) / sizeof(rollup->pairs[0]))
		return false;
	size_t tail = (rollup->head + rollup->count) % (sizeof(rollup->pairs) / sizeof(rollup->pairs[0]));
	rollup->pairs[tail] = pair;
	++rollup->count;
	return true;
}

static bool push_control_locked(struct captions_rollup *rollup, eia608_control_t command)
{
	uint16_t pair = eia608_control_command(command, 0);
	return push_locked(rollup, pair) && push_locked(rollup, pair);
}

static void reset_locked(struct captions_rollup *rollup)
{
	rollup->head = 0;
	rollup->count = 0;
	rollup->column = 0;
	push_control_locked(rollup, eia608_control_erase_display_memory);
	push_control_locked(rollup, eia608_control_roll_up_2);
	uint16_t pac = eia608_row_column_pramble(ROLLUP_BOTTOM_ROW, 0, 0, 0);
	push_locked(rollup, pac);
	push_locked(rollup, pac);
}

bool captions_rollup_init(struct captions_rollup *rollup)
{
	memset(rollup, 0, sizeof(*rollup));
	return pthread_mutex_init(&rollup->mutex, NULL) == 0;
}

void captions_rollup_destroy(struct captions_rollup *rollup)
{
	pthread_mutex_destroy(&rollup->mutex);
}

void captions_rollup_reset(struct captions_rollup *rollup)
{
	pthread_mutex_lock(&rollup->mutex);
	reset_locked(rollup);
	pthread_mutex_unlock(&rollup->mutex);
}

void captions_rollup_clear(struct captions_rollup *rollup)
{
	pthread_mutex_lock(&rollup->mutex);
	rollup->head = 0;
	rollup->count = 0;
	rollup->column = 0;
	pthread_mutex_unlock(&rollup->mutex);
}

static bool queue_word_locked(struct captions_rollup *rollup, const char *word)
{
	char chars[256][2];
	size_t length = 0;
	for (const unsigned char *p = (const unsigned char *)word; *p; ++p) {
		if (*p >= 0x80 || length == sizeof(chars) / sizeof(chars[0]))
			return false;
		chars[length][0] = (char)*p;
		chars[length][1] = '\0';
		++length;
	}
	if (!length)
		return true;

	if (rollup->column && rollup->column + 1 + length > ROLLUP_COLUMNS) {
		if (!push_control_locked(rollup, eia608_control_carriage_return))
			return false;
		rollup->column = 0;
	}
	if (rollup->column) {
		char space[] = " ";
		uint16_t pair = eia608_from_utf8_1(space, 0);
		if (!pair || !push_locked(rollup, pair))
			return false;
		++rollup->column;
	}

	for (size_t start = 0; start < length;) {
		size_t count = ROLLUP_COLUMNS - rollup->column;
		if (count > length - start)
			count = length - start;
		for (size_t i = 0; i < count; i += 2) {
			uint16_t pair = i + 1 < count ? eia608_from_utf8_2(chars[start + i], chars[start + i + 1])
						      : eia608_from_utf8_1(chars[start + i], 0);
			if (!pair || !push_locked(rollup, pair))
				return false;
		}
		start += count;
		rollup->column += count;
		if (start < length) {
			if (!push_control_locked(rollup, eia608_control_carriage_return))
				return false;
			rollup->column = 0;
		}
	}
	return true;
}

bool captions_rollup_word(struct captions_rollup *rollup, const char *word)
{
	if (!word)
		return false;
	pthread_mutex_lock(&rollup->mutex);
	bool ok = queue_word_locked(rollup, word);
	pthread_mutex_unlock(&rollup->mutex);
	return ok;
}

bool captions_rollup_pop(struct captions_rollup *rollup, uint16_t *pair)
{
	pthread_mutex_lock(&rollup->mutex);
	bool available = rollup->count > 0;
	if (available) {
		*pair = rollup->pairs[rollup->head];
		rollup->head = (rollup->head + 1) % (sizeof(rollup->pairs) / sizeof(rollup->pairs[0]));
		--rollup->count;
	}
	pthread_mutex_unlock(&rollup->mutex);
	return available;
}
