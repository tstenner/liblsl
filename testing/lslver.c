#include "../include/lsl_c.h"
#include <stdio.h>

int main() {
	printf("LSL version: %d\n\n%f\n", lsl_library_version(), lsl_local_clock());
	return 0;
}
