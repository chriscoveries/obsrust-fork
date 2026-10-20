#include "speech-transcriber.h"

#include "captions-filter.h"

extern void *mac_captions_speech_create(void *factory_data);
extern bool mac_captions_speech_start(void *session, uint32_t sample_rate,
				      void (*word)(void *, const char *, double, double), void *param);
extern bool mac_captions_speech_push(void *session, const float *samples, uint32_t frames, uint64_t timestamp_ns);
extern void mac_captions_speech_stop(void *session);
extern void mac_captions_speech_destroy(void *session);
extern int32_t mac_captions_speech_transcribe_file(const char *path, void (*word)(void *, const char *, double, double),
						   void *param);

const struct mac_captions_transcriber_ops mac_captions_speech_transcriber_ops = {
	.create = mac_captions_speech_create,
	.start = mac_captions_speech_start,
	.push = mac_captions_speech_push,
	.stop = mac_captions_speech_stop,
	.destroy = mac_captions_speech_destroy,
};
