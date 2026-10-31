/* Forwarding file (BUILD.md): the test/cmocka/test_vt_*.c tests include
 * this source by its old name, and it brings the plane copy the encoder
 * calls. The build compiles encoder.c.h and vt-frame-copy.c.h through
 * their unity group, never this file. */
#include "plugins/mac-videotoolbox/encoder.c.h"
#include "plugins/mac-videotoolbox/vt-frame-copy.c.h"
