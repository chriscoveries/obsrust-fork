#include "plugins/mac-videotoolbox/build/mac-videotoolbox.pch.h"

size_t vt_copy_frame_to_pixbuf(CVPixelBufferRef pixbuf, uint8_t *data[MAX_AV_PLANES], uint32_t linesize[MAX_AV_PLANES])
{
	size_t copied = 0;

	for (int i = 0; i < MAX_AV_PLANES; i++) {
		if (data[i] == NULL)
			break;
		uint8_t *p = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixbuf, i);
		uint8_t *f = data[i];
		size_t plane_linesize = CVPixelBufferGetBytesPerRowOfPlane(pixbuf, i);
		size_t plane_height = CVPixelBufferGetHeightOfPlane(pixbuf, i);

		for (size_t j = 0; j < plane_height; j++) {
			memcpy(p, f, linesize[i]);
			p += plane_linesize;
			f += linesize[i];
		}
		copied += linesize[i] * plane_height;
	}

	return copied;
}
