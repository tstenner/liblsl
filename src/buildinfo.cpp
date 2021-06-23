extern "C" {
#include "../include/lsl/common.h"

#ifdef LSL_CMAKE_INFO
#include "buildinfo.h"
#else
#ifndef LSL_LIBRARY_INFO_STR
#define LSL_LIBRARY_INFO_STR "Unknown (not set by build system)"
#endif
#endif

LIBLSL_C_API const char *lsl_library_info(void) {
	return LSL_LIBRARY_INFO_STR;
}
}
