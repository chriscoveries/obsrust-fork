#pragma once

#include <stdbool.h>
#include <stdint.h>

struct mac_captions_transcriber_ops;

enum mac_captions_asset_status {
	MAC_CAPTIONS_ASSETS_UNSUPPORTED = 0,
	MAC_CAPTIONS_ASSETS_DOWNLOADING = 1,
	MAC_CAPTIONS_ASSETS_SUPPORTED = 2,
	MAC_CAPTIONS_ASSETS_INSTALLED = 3,
};

int32_t mac_captions_speech_asset_status(void);
int32_t mac_captions_speech_transcribe_file(
	const char *path, void (*word)(void *param, const char *text, double start_sec, double end_sec), void *param);
extern const struct mac_captions_transcriber_ops mac_captions_speech_transcriber_ops;
