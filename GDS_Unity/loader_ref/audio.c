/* audio.c -- GDS libunity imports only `fmodf` (a libm math symbol), NOT the
 * FMOD audio backend.  So the FMOD/AudioTrack loop the reference (Hitman GO)
 * needs is irrelevant here; gds_audio_start/stop are no-ops. */
#include "gds.h"
int gds_audio_start(void *env) { (void)env; return 1; }
void gds_audio_stop(void) { }
