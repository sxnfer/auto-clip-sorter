#include "motion-preference.hpp"

#include <windows.h>

namespace clip_atlas {
bool prefers_reduced_motion()
{
	BOOL enabled = TRUE;
	return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) && !enabled;
}
} // namespace clip_atlas
