#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("mac-captions", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "On-device live captions for macOS";
}

extern struct obs_source_info mac_live_captions_filter;

bool obs_module_load(void)
{
	obs_register_source(&mac_live_captions_filter);
	return true;
}
