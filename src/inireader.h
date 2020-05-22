#include "cast.h"
#include <string>
#include <unordered_map>

// Reads an INI file from a stream into a map
class INI {
	std::unordered_map<std::string, std::string> values;

	template <typename T> inline T convert(const std::string &val) const {
		return lsl::from_string<T>(val);
	}

public:
	void load(std::istream &ini);

	template <typename T> inline const T get(const std::string &key, T defaultval = T()) const {
		auto it = values.find(key);
		if (it == values.end())
			return defaultval;
		else
			return convert<T>(it->second);
	}
};

template <> inline const char *INI::convert(const std::string &val) const { return val.c_str(); }

template <> inline const std::string &INI::convert(const std::string &val) const { return val; }

template <> inline std::string INI::convert(const std::string &val) const { return val; }
