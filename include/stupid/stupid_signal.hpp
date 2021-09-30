#pragma once

#include <cstdint>
#include <functional>
#include <map>

namespace stupid {

class SyncSignal
{
public:

	std::uint32_t value() const { return value_; }
	void operator()() { value_++; }

private:

	std::uint32_t value_ = 0;
};

} // stupid