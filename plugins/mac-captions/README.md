# Live captions for macOS

The audio filter is registered as `mac_live_captions_filter` and is labeled
“Live captions (on-device)”. Enabling the filter starts on-device
`SpeechTranscriber` recognition of the filtered source; disabling it stops the
recognizer. It does not add settings keys. Finalized words are encoded as
CEA-608 roll-up captions and sent to active outputs through `obs_output_caption`.
The enabled session reserves its selected locale for the duration of
recognition, then releases it. The filter never requests or installs assets.

The first version uses the en-US speech model on macOS 27 or later. It reads
audio supplied by OBS and does not request microphone access or install speech
assets automatically. The headless real-speech test skips with the asset status
when en-US is not installed; fixture-driven fake-transcriber coverage does not
need the model.

To install the en-US asset manually, build and run the separate installer:

```sh
cmake --build <build-dir> --target mac-captions-install-speech-assets
<build-dir>/test/cmocka/install-speech-assets --install-en-us-speech-assets
```

The installer reserves the resolved en-US locale before requesting its asset.
It is a separate executable and requires the explicit flag; the filter never
requests or installs assets.
