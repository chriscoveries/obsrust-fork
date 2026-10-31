# HLG-compatible HDR signaling in Hybrid MP4: with the experimental
# write_hlg_compatible_hdr_config muxer option, the encoder's configuration
# record (obs_encoder_get_codec_metadata) is written as a dvvC box right after
# hvcC, and nothing changes without the option or without a valid record.
# Builds obs-outputs' mp4-output.c and mp4-mux.c into the test and muxes stub
# HEVC encoder packets into a file; obs_startup needs a display, so it runs
# (under xvfb-run) on Linux and skips without one.
# Defined in ../build/test-graph.json.
