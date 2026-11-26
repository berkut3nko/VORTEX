/* Plain C translation unit compiling miniaudio and stb_vorbis once for the engine (docs/design/audio.md
 * "Decision"). stb_vorbis's declarations come first so that miniaudio enables its Ogg Vorbis decoder; the
 * stb_vorbis implementation follows miniaudio's, as miniaudio's documentation asks. */
#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"
