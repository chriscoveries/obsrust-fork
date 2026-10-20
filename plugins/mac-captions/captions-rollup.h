#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

struct captions_rollup {
	pthread_mutex_t mutex;
	uint16_t pairs[4096];
	size_t head;
	size_t count;
	size_t column;
};

bool captions_rollup_init(struct captions_rollup *rollup);
void captions_rollup_destroy(struct captions_rollup *rollup);
void captions_rollup_reset(struct captions_rollup *rollup);
void captions_rollup_clear(struct captions_rollup *rollup);
bool captions_rollup_word(struct captions_rollup *rollup, const char *word);
bool captions_rollup_pop(struct captions_rollup *rollup, uint16_t *pair);
