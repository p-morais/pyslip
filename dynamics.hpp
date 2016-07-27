#pragma once

#include "State.hpp"
#include "Environment.hpp"
#include <vector>

namespace sim
{

TimeState integration_step(TimeState ts,
	double dt,
	float l_torque,
	float theta_torque);


TimeState get_initial_timestate();

} // namespace sim
