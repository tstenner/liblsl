#include <thread>
#include <iostream>
#include <lsl_cpp.h>
#include <random>


/**
 * This example program offers a 1-channel stream which contains strings.
 * The stream has the "Marker" content type and irregular rate.
 * The name of the stream can be chosen as a startup parameter.
 *
 * Each marker is sent three times: with a negative time offset, "immediate"
 * and with a positive time offset, i.e. in the future.
 * Compliant stream visualizers / recording software should display those
 * markers exactly 150ms apart
 */

int main(int argc, char* argv[]) {
	try {
		const char* name = argc > 1 ? argv[1] : "MyEventStream";
		// make a new stream_info
		lsl::stream_info info(name, "Markers", 1, lsl::IRREGULAR_RATE, lsl::cf_string, "id23443");

		// make a new outlet
		lsl::stream_outlet outlet(info);

		// send random marker strings
		std::cout << "Now sending markers... " << std::endl;
		std::vector<std::string> markertypes{"Test", "Blah", "Marker", "XXX", "Testtest", "Test-1-2-3"};
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<std::size_t> rnd(0, markertypes.size() - 1);
		std::uniform_int_distribution<int> delayrnd(400, 1000);
		while(true) {
			// wait for a variable delay
			auto delay = std::chrono::milliseconds(delayrnd(gen));
			std::this_thread::sleep_for(delay);
			// and choose the marker to send
			std::string mrk = markertypes[rnd(gen)];
			std::cout << "now sending: 3x " << mrk << std::endl;
			double t = lsl::local_clock();
			for (double offset : {-.2, 0., .2}) {
				std::string sendmarker = mrk + ' ' + std::to_string(offset);
				// now send it (note the &)
				outlet.push_sample(&mrk, t + offset);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				std::cout << "now sending: " << sendmarker << std::endl;
			}
		}
	} catch (std::exception& e) { std::cerr << "Got an exception: " << e.what() << std::endl; }
	std::cout << "Press any key to exit. " << std::endl;
	std::cin.get();
	return 0;
}
