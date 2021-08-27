#include <lsl_c.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/// Send several streams that can be used to test protocol conformance

enum streams { string = 0, int32, double64, n_streams };

int main() {
	char buf[15];

	printf("Using lsl %d\n", lsl_library_version());

	lsl_streaminfo info[n_streams];
	sprintf(buf, "%d", lsl_library_version());
	info[string] = lsl_create_streaminfo(buf, "Test", 2, 1, cft_string, "");
	sprintf(buf, "int32_v%d", lsl_library_version());
	info[int32] = lsl_create_streaminfo(buf, "Test", 3, 1, cft_int32, "");
	sprintf(buf, "double64_v%d", lsl_library_version());
	info[double64] = lsl_create_streaminfo(buf, "Test", 2, 1, cft_double64, "");

	lsl_outlet outlet[n_streams];
	for (int i = 0; i < n_streams; ++i) {
		lsl_xml_ptr desc = lsl_get_desc(info[i]);
		lsl_append_child_value(desc, "manufacturer", "LSL");
		lsl_xml_ptr chns = lsl_append_child(desc, "channels");
		lsl_append_child_value(chns, "name", "Channel 1");
		lsl_append_child_value(chns, "name", "Channel 2");
		outlet[i] = lsl_create_outlet(info[i], 0, 360);
	}

	const char buf2[] = "\x00Hello World";
	const char *strbufs[2] = {buf, buf2};
	uint32_t strlens[2] = {15, sizeof(buf2)};

	int32_t intbuf[3] = {0};
	double doublebuf[2] = {0};
	buf[0] = 0;
	while (1) {
		double t = lsl_local_clock();
		sprintf(buf + 1, "\n%d", (int)t);
		lsl_push_sample_buftp(outlet[string], strbufs, strlens, t, 1);

		intbuf[0] = (int)t;
		intbuf[1] = (int)-t;
		intbuf[2] = ~((int)t);
		lsl_push_sample_itp(outlet[int32], intbuf, t, 1);

		doublebuf[0] = t;
		doublebuf[1] = -t;
		lsl_push_sample_dtp(outlet[int32], doublebuf, t, 1);
		printf("Sent samples @t=%f\n", t);
		sleep(1);
	}
	return 0;
}
