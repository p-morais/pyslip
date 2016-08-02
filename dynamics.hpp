#pragma once

#include "State.hpp"
#include "Environment.hpp"
#include <vector>

#include <boost/python/numeric.hpp>

namespace sim
{

State run_timestep(boost::python::numeric::array state,
				   double dt,
			   	   float l_torque,
				   float theta_torque);

State get_initial_s();

} // namespace sim
