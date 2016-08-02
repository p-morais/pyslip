#define BOOST_PYTHON_STATIC_LIB
#include <boost\python.hpp>
#include <iostream>
#include "sim.hpp"

BOOST_PYTHON_MODULE(pyslip_wrap)
{
	using namespace boost::python;

	class_<sim::State>("State")
		.def_readwrite("x", &sim::State::x)
		.def_readwrite("y", &sim::State::y)
		.def_readwrite("phi", &sim::State::phi)
		.def_readwrite("l", &sim::State::l)
		.def_readwrite("l_eq", &sim::State::l_eq)
		.def_readwrite("theta", &sim::State::theta)
		.def_readwrite("theta_eq", &sim::State::theta_eq)
		.def_readwrite("dx", &sim::State::dx)
		.def_readwrite("dy", &sim::State::dy)
		.def_readwrite("dphi", &sim::State::dphi)
		.def_readwrite("dl", &sim::State::dl)
		.def_readwrite("dl_eq", &sim::State::dl_eq)
		.def_readwrite("dtheta", &sim::State::dtheta)
		.def_readwrite("dtheta_eq", &sim::State::dtheta_eq);

	class_<sim::TimeState>("TimeState")
		.def_readwrite("time", &sim::TimeState::time)
		.def_readwrite("state", &sim::TimeState::state);

	def("step", sim::run_timestep);

	def("reset", sim::get_initial_s);
}