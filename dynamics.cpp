#include "dynamics.hpp"
#include <array>
#include <cmath>
#include <vector>
#include <limits>
#include <cstddef>

#include <boost/python/extract.hpp>
#include <boost/python/numeric.hpp>
#include <boost/python.hpp>

#include <iostream>

#define DELTA_T 0.0001
namespace sim
{

/*******************************************************************************
 * Private functions and data structures
 ******************************************************************************/

namespace
{

	namespace py = boost::python;
/*******************************************************************************
 * Data structures
 ******************************************************************************/

struct ExternalForces
{
    double body_x;
    double body_y;
    double body_phi;
    double foot_x;
    double foot_y;
};


struct MotorTorques
{
    double length;
    double angle;
};


struct Force
{
    double x;
    double y;
};


struct PointState
{
    double x;
    double y;
    double dx;
    double dy;
};


inline MotorTorques operator+ (const MotorTorques& a, const MotorTorques& b)
{
    return {a.length + b.length, a.angle + b.angle};
}


/*******************************************************************************
 * Utility functions
 ******************************************************************************/

inline State np_to_state(boost::python::numeric::array data)
{
	return{ py::extract<double>(data[0]),
			py::extract<double>(data[1]),
			py::extract<double>(data[2]),
			py::extract<double>(data[3]),
			py::extract<double>(data[4]),
			py::extract<double>(data[5]),
			py::extract<double>(data[6]),

			py::extract<double>(data[7]),
			py::extract<double>(data[8]),
			py::extract<double>(data[9]),
			py::extract<double>(data[10]),
			py::extract<double>(data[11]),
			py::extract<double>(data[12]),
			py::extract<double>(data[13]) };
}

inline double clamp(double x, double lower, double upper)
{
    // Clamp x to the lower and upper bounds
    return std::fmin(std::fmax(x, lower), upper);
}


inline double fade_derivative(double x, double lower, double upper, double fade)
{
    // Returns 1 when outside of lower and upper bounds, fades to 0
    // along distance fade within bounds
    const double x_over = x - clamp(x, lower + fade, upper - fade);
    return clamp(std::fabs(x_over / fade), 0, 1);
}


inline double pd_controller(double err, double derr, double kp, double kd)
{
    return (kp * err) + (kd * derr);
}



/*******************************************************************************
 * Equations of motion
 ******************************************************************************/

inline DState hopper_eom(State state,
                         const Environment& env,
                         MotorTorques motors,
                         ExternalForces ext)
{
    // Calculate motor gap torques, taking damping into account
    const double angle_motor_gap_torque = motors.angle -
        (env.angle_motor_damping * state.dtheta_eq * env.angle_motor_ratio);
    const double length_motor_gap_torque = motors.length -
        (env.length_motor_damping * state.dl_eq * env.length_motor_ratio);

    // Calculate internal spring forces
    const double length_spring_force =
        (env.length_stiffness * (state.l_eq - state.l)) +
        (env.length_damping * (state.dl_eq - state.dl));
    const double angle_spring_torque =
        (env.angle_stiffness * (state.theta_eq - state.theta)) +
        (env.angle_damping * (state.dtheta_eq - state.dtheta));
    const double angle_spring_force = angle_spring_torque / state.l;

    // Get basis vectors for internal spring forces
    // Positive when acting on the foot, negate for body
    const double l_x = std::sin(state.theta + state.phi);
    const double l_y = -std::cos(state.theta + state.phi);
    const double theta_x = -l_y;
    const double theta_y = l_x;

    // Forces on body
    const double force_body_x = ext.body_x - (l_x * length_spring_force) -
        (theta_x * angle_spring_force);
    const double force_body_y = ext.body_y - (l_y * length_spring_force) -
        (theta_y * angle_spring_force);
    const double torque_body_phi = ext.body_phi - angle_motor_gap_torque -
        (1.0 - 1.0 / env.angle_motor_ratio) * angle_spring_torque;

    // Acceleration of body
    const double ddx = force_body_x / env.body_mass;
    const double ddy = force_body_y / env.body_mass;
    const double ddphi = torque_body_phi / env.body_inertia;

    // Acceleration of leg equilibrium positions
    const double ddtheta_eq = (angle_motor_gap_torque -
                               angle_spring_torque / env.angle_motor_ratio) /
        (env.angle_motor_ratio * env.angle_motor_inertia);
    const double ddl_eq = (length_motor_gap_torque -
                           length_spring_force / env.length_motor_ratio) /
        (env.length_motor_ratio * env.length_motor_inertia);

    // Convert external forces on foot to relative polar coordinate acceleration
    // Gravity is included in the external forces
    const double accel_offset_foot_x = ext.foot_x / env.foot_mass - ddx;
    const double accel_offset_foot_y = ext.foot_y / env.foot_mass - ddy;
    const double accel_foot_l = (length_spring_force / env.foot_mass) +
        (accel_offset_foot_x * l_x) + (accel_offset_foot_y * l_y);
    const double accel_foot_theta = (angle_spring_force / env.foot_mass) +
        (accel_offset_foot_x * theta_x) + (accel_offset_foot_y * theta_y);

    // Acceleration of actual leg positions
    const double dtheta_abs = state.dtheta + state.dphi;
    const double ddl = accel_foot_l + (state.l * dtheta_abs * dtheta_abs);
    const double ddtheta = (accel_foot_theta -
                            (2 * state.dl * dtheta_abs)) / state.l - ddphi;

    // Output state derivative vector
    return {state.dx,
            state.dy,
            state.dphi,
            state.dl,
            state.dl_eq,
            state.dtheta,
            state.dtheta_eq,
            ddx,
            ddy,
            ddphi,
            ddl,
            ddl_eq,
            ddtheta,
            ddtheta_eq};
}


inline Force ground_contact_model(PointState point, const Environment& env)
{
    // Find the point on the ground closest to the point to test
    double min_dist2 = std::numeric_limits<double>::max();
    double min_p = 0.0;
    double min_x_line = 0.0;
    double min_y_line = 0.0;
    double min_seg_length2 = 0.0;
    size_t min_index = 0;
    for (size_t i = 0; i < env.ground.size() - 1; ++i)
    {
        const double xg = env.ground[i].x;
        const double yg = env.ground[i].y;
        const double dxg = env.ground[i + 1].x - xg;
        const double dyg = env.ground[i + 1].y - yg;

        // Take dot product to project test point onto line, then
        // normalize with the segment length squared and clamp to keep
        // within line segment bounds
        const double dot_product = (point.x - xg) * dxg + (point.y - yg) * dyg;
        const double seg_length2 = (dxg * dxg) + (dyg * dyg);
        const double p = clamp(dot_product / seg_length2, 0, 1);

        // Nearest point on the line segment to the test point
        const double x_line = xg + (p * dxg);
        const double y_line = yg + (p * dyg);

        // Squared distance from line point to test point
        const double dist2 = ((point.x - x_line) * (point.x - x_line)) +
            ((point.y - y_line) * (point.y - y_line));

        // If this is a new minimum, save values
        // Ignore segments with zero length
        if (dist2 < min_dist2 && seg_length2 > 0.0)
        {
            min_dist2 = dist2;
            min_p = p;
            min_x_line = x_line;
            min_y_line = y_line;
            min_seg_length2 = seg_length2;
            min_index = i;
        }
    }

    // Check whether point is on the ground side (right hand side) of the line
    // If not, return immediately with zero ground reaction force
    const double dxg = env.ground[min_index + 1].x - env.ground[min_index].x;
    const double dyg = env.ground[min_index + 1].y - env.ground[min_index].y;
    const double dxp = point.x - env.ground[min_index].x;
    const double dyp = point.y - env.ground[min_index].y;
    const double cross_product = (dxg * dyp) - (dyg * dxp);
    if (cross_product > 0.0)
        return {0.0, 0.0};

    // If the point is a vertex, also check the next line
    if (min_p == 1.0 && min_index < env.ground.size() - 2)
    {
        const double dxg = env.ground[min_index + 2].x -
            env.ground[min_index + 1].x;
        const double dyg = env.ground[min_index + 2].y -
            env.ground[min_index + 1].y;
        const double dxp = point.x - env.ground[min_index + 1].x;
        const double dyp = point.y - env.ground[min_index + 1].y;
        const double cross_product = (dxg * dyp) - (dyg * dxp);
        if (cross_product > 0.0)
            return {0.0, 0.0};
    }

    // If execution reaches here, the point is in the ground
    // Note that if the test point is outside the bounds of the
    // polyline, it is handled incorrectly

    // Get normal and tangent basis vectors
    // NOTE: Normal is into ground, tangent is 90 deg CCW from normal
    const double depth = std::sqrt(min_dist2);
    double tangent_x, tangent_y, normal_x, normal_y;
    if (min_p == 0.0 || min_p == 1.0)
    {
        // Special case for corners -- normal is aligned with vector
        // from test point to corner
        normal_x = -(point.x - min_x_line) / depth;
        normal_y = -(point.y - min_y_line) / depth;
        tangent_x = normal_y;
        tangent_y = -normal_x;
    }
    else
    {
        // Typical case -- use segment direction for tangent
        const double seg_length = std::sqrt(min_seg_length2);
        tangent_x = dxg / seg_length;
        tangent_y = dyg / seg_length;
        normal_x = -tangent_y;
        normal_y = tangent_x;
    }

    // Get derivative of depth
    const double ddepth = (-normal_x * point.dx) + (-normal_y * point.dy);

    // Get interpolated ground properties
    const double ground_stiffness = env.ground[min_index].stiffness +
        (min_p * (env.ground[min_index + 1].stiffness -
                  env.ground[min_index].stiffness));
    const double ground_damping = env.ground[min_index].damping +
        (min_p * (env.ground[min_index + 1].damping -
                  env.ground[min_index].damping));
    const double ground_friction = env.ground[min_index].friction +
        (min_p * (env.ground[min_index + 1].friction -
                  env.ground[min_index].friction));

    // Damping adjustment factor
    const double damping_factor = depth / (depth + env.ground_damping_depth);

    // Normal force (spring + damper) should only be positive upwards
    const double normal_force =
        std::fmax((depth * ground_stiffness) +
                  (ddepth * damping_factor * ground_damping), 0.0);

    // Tangent force (friction) before finding sign and smoothing discontinuity
    const double friction_max = ground_friction * normal_force;
    const double tangent_velocity = (tangent_x * point.dx) +
        (tangent_y * point.dy);
    const double viscous_friction_factor =
        clamp(tangent_velocity / (friction_max * env.ground_slip_ramp), -1, 1);
    const double tangent_force = -viscous_friction_factor * friction_max;

    return {(normal_x * normal_force) + (tangent_x * tangent_force),
            (normal_y * normal_force) + (tangent_y * tangent_force)};
}


inline MotorTorques hardstop_forces(State state, const Environment& env)
{
    // Compute how much each DOF is over/under the hardstops
    const double l_eq_over = state.l_eq -
        clamp(state.l_eq, env.length_min, env.length_max);
     const double theta_eq_over = state.theta_eq -
        clamp(state.theta_eq, env.angle_min, env.angle_max);

    // Fade the derivative term in near the hardstops for smoother dynamics
    const double l_eq_dfade = fade_derivative(state.l_eq,
                                              env.length_min,
                                              env.length_max,
                                              env.length_hardstop_dfade);
    const double theta_eq_dfade = fade_derivative(state.theta_eq,
                                                  env.angle_min,
                                                  env.angle_max,
                                                  env.angle_hardstop_dfade);

    // Compute hardstop forces as a spring+damper system
    const double l_eq_force = -(env.length_hardstop_kp * l_eq_over) -
        (env.length_hardstop_kd * l_eq_dfade * state.dl_eq);
    const double theta_eq_torque = -(env.angle_hardstop_kp * theta_eq_over) -
        (env.angle_hardstop_kd * theta_eq_dfade * state.dtheta_eq);

    // Clamp forces before returning them
    return {clamp(l_eq_force,
                  -env.length_hardstop_fmax,
                  env.length_hardstop_fmax),
            clamp(theta_eq_torque,
                  -env.angle_hardstop_fmax,
                  env.angle_hardstop_fmax)};
}


inline DState hopper_dynamics(State state,
                              float l_torque,
                              float theta_torque)
{

	sim::Environment env = { 10, 0.3, 0.4, 1e4, 1e2, 1e-3, 1e-1, 32, 0.3, 1, 1e4, 1e2, 1e-3, 1e-1, 16, -1.5, 1.5, 9.81,
		std::vector<sim::GroundVertex>{ {-1e3,0,1e5,4e2,1},{ 1e3,0,1e5,4e2,1 }}, 1e-4, 4e3, 4e1, 1e-2,
		1e5, 1e3, 1e2, 1e-2, 1e3, 1e-3, 1e-4 };

	const MotorTorques motors = { clamp(l_torque,     -1e2, 1e2),
								  clamp(theta_torque, -1e2, 1e2) };

    // Get hardstop forces
    const MotorTorques hardstops = hardstop_forces(state, env);

    // Calculate external forces
    const double l_x = std::sin(state.theta + state.phi);
    const double l_y = -std::cos(state.theta + state.phi);
    const double theta_x = -l_y;
    const double theta_y = l_x;
    const double dtheta_abs = state.dtheta + state.dphi;
    const PointState foot_point = {
        state.x + (l_x * state.l),
        state.y + (l_y * state.l),
        state.dx + (l_x * state.dl) + (theta_x * state.l * dtheta_abs),
        state.dy + (l_y * state.dl) + (theta_y * state.l * dtheta_abs)};
    const Force foot_ground_force = ground_contact_model(foot_point, env);

    const ExternalForces ext = {
        0.0,
        -(env.body_mass * env.gravity),
        0.0,
        foot_ground_force.x,
        foot_ground_force.y - (env.foot_mass * env.gravity)};

    return hopper_eom(state, env, motors + hardstops, ext);
}


inline bool detect_flight(State state, const Environment& env)
{
    const double l_comp = state.l_eq - state.l;
    return l_comp < 0.005;
}


inline bool detect_stance(State state, const Environment& env)
{
    const double l_comp = state.l_eq - state.l;
    return l_comp > 0.01 && state.dl < 0.0;
}

State integration_step(State s,
	double dt,
	float l_torque,
	float theta_torque)
{
	// Update the state of the low-level controller once per major step

	// Performs a 4th order runge-kutta integration step
	// dt is passed explicitly instead of using env.dt so that the
	// integrator can take a short final timestep
	const State   s0 = s;
	const DState ds0 = hopper_dynamics(s0, l_torque, theta_torque);

	const State   s1 = s0 + ds0 * (dt / 2);
	const DState ds1 = hopper_dynamics(s1, l_torque, theta_torque);

	const State   s2 = s0 + ds1 * (dt / 2);
	const DState ds2 = hopper_dynamics(s2, l_torque, theta_torque);

	const State   s3 = s0 + ds2 * dt;
	const DState ds3 = hopper_dynamics(s3, l_torque, theta_torque);

	return{ s0 + (ds0 + 2 * ds1 + 2 * ds2 + ds3) * (dt / 6) };
}

} // namespace


/*******************************************************************************
 * Public functions
 ******************************************************************************/
State run_timestep(py::numeric::array state,
				   double dt,
				   float l_thorque,
				   float theta_thorque)
{
	State s = np_to_state(state);

	for (int t = 0; t < dt; t += DELTA_T)
		s = integration_step(s, DELTA_T, l_thorque, theta_thorque);
	
	return s;
}


State get_initial_s()
{
	return{ 2.0, 0.7, 0.0, 0.7, 0.7, 0.3, 0.3,
			1.0, -0.5, 0.0, 0.0, 0.0, 0.0, 0.0 };
}

} // namespace sim
