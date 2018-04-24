/*!
 * @file
 *
 * @section LICENSE
 *
 * Copyright (C) 2017 by the Georgia Tech Research Institute (GTRI)
 *
 * This file is part of SCRIMMAGE.
 *
 *   SCRIMMAGE is free software: you can redistribute it and/or modify it under
 *   the terms of the GNU Lesser General Public License as published by the
 *   Free Software Foundation, either version 3 of the License, or (at your
 *   option) any later version.
 *
 *   SCRIMMAGE is distributed in the hope that it will be useful, but WITHOUT
 *   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 *   License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with SCRIMMAGE.  If not, see <http://www.gnu.org/licenses/>.
 *
 * @author Kevin DeMarco <kevin.demarco@gtri.gatech.edu>
 * @author Eric Squires <eric.squires@gtri.gatech.edu>
 * @date 31 July 2017
 * @version 0.1.0
 * @brief Brief file description.
 * @section DESCRIPTION
 * A Long description goes here.
 *
 */

#include <scrimmage/plugins/motion/JSBSimControl/JSBSimControl.h>

#include <initialization/FGTrim.h>

#include <scrimmage/common/Utilities.h>
#include <scrimmage/parse/ParseUtils.h>
#include <scrimmage/parse/MissionParse.h>
#include <scrimmage/math/Angles.h>
#include <scrimmage/math/State.h>
#include <scrimmage/entity/Entity.h>
#include <scrimmage/plugin_manager/RegisterPlugin.h>
#include <scrimmage/proto/Shape.pb.h>
#include <scrimmage/proto/ProtoConversions.h>

#include <iomanip>
#include <iostream>

#include <boost/algorithm/clamp.hpp>
#include <GeographicLib/LocalCartesian.hpp>

using std::cout;
using std::cerr;
using std::endl;

#define meters2feet 3.28084
#define feet2meters (1.0 / meters2feet)

REGISTER_PLUGIN(scrimmage::MotionModel, scrimmage::motion::JSBSimControl, JSBSimControl_plugin)

namespace scrimmage {
namespace motion {

namespace sc = scrimmage;
using ang = scrimmage::Angles;

namespace ba = boost::algorithm;

JSBSimControl::JSBSimControl() {
    angles_from_jsbsim_.set_input_clock_direction(ang::Rotate::CW);
    angles_from_jsbsim_.set_input_zero_axis(ang::HeadingZero::Pos_Y);
    angles_from_jsbsim_.set_output_clock_direction(ang::Rotate::CCW);
    angles_from_jsbsim_.set_output_zero_axis(ang::HeadingZero::Pos_X);

    angles_to_jsbsim_.set_input_clock_direction(ang::Rotate::CCW);
    angles_to_jsbsim_.set_input_zero_axis(ang::HeadingZero::Pos_X);
    angles_to_jsbsim_.set_output_clock_direction(ang::Rotate::CW);
    angles_to_jsbsim_.set_output_zero_axis(ang::HeadingZero::Pos_Y);
}

std::tuple<int, int, int> JSBSimControl::version() {
    return std::tuple<int, int, int>(0, 0, 1);
}

bool JSBSimControl::init(std::map<std::string, std::string> &info,
                         std::map<std::string, std::string> &params) {


    drawVel_ = sc::get<double>("drawVel", params, 1.0);
    drawAngVel_ = sc::get<double>("drawAngVel", params, 10.0);
    drawAcc_ = sc::get<double>("drawAcc", params, 1.0);

    // Setup variable index for controllers
    thrust_idx_ = vars_.declare(VariableIO::Type::thrust, VariableIO::Direction::In);
    elevator_idx_ = vars_.declare(VariableIO::Type::elevator, VariableIO::Direction::In);
    aileron_idx_ = vars_.declare(VariableIO::Type::aileron, VariableIO::Direction::In);
    rudder_idx_ = vars_.declare(VariableIO::Type::rudder, VariableIO::Direction::In);

    roll_pid_.set_parameters(std::stod(params["roll_kp"]),
                             std::stod(params["roll_ki"]),
                             std::stod(params["roll_kd"]));

    roll_pid_.set_integral_band(M_PI/40.0);
    roll_pid_.set_is_angle(true);

    //////////////////////

    pitch_pid_.set_parameters(std::stod(params["pitch_kp"]),
                             std::stod(params["pitch_ki"]),
                             std::stod(params["pitch_kd"]));

    pitch_pid_.set_integral_band(M_PI/40.0);
    pitch_pid_.set_is_angle(true);

    /////////////////////

    yaw_pid_.set_parameters(std::stod(params["yaw_kp"]),
                             std::stod(params["yaw_ki"]),
                             std::stod(params["yaw_kd"]));

    yaw_pid_.set_integral_band(M_PI/40.0);
    yaw_pid_.set_is_angle(true);

    //////////

    exec = std::make_shared<JSBSim::FGFDMExec>();

    exec->SetDebugLevel(1);
    exec->SetRootDir(info["JSBSIM_ROOT"]);
    exec->SetAircraftPath("/aircraft");
    exec->SetEnginePath("/engine");
    exec->SetSystemsPath("/systems");

    exec->LoadScript("/scripts/"+info["script_name"]);

    exec->SetRootDir(parent_->mp()->log_dir());
    exec->SetRootDir(info["JSBSIM_ROOT"]);

    JSBSim::FGInitialCondition *ic = exec->GetIC();
    ic->SetVEastFpsIC(state_->vel()[1] * meters2feet);
    ic->SetVNorthFpsIC(state_->vel()[0] * meters2feet);
    ic->SetVDownFpsIC(-state_->vel()[2] * meters2feet);

    // TODO: add heading
    ic->SetTerrainElevationFtIC(parent_->projection()->HeightOrigin() * meters2feet);
    ic->SetLatitudeDegIC(parent_->projection()->LatitudeOrigin() );
    ic->SetLongitudeDegIC(parent_->projection()->LongitudeOrigin() );
    ic->SetAltitudeASLFtIC((parent_->projection()->HeightOrigin()+state_->pos()[2]) * meters2feet);

    exec->RunIC();
    exec->Setdt(std::stod(info["dt"]));
    exec->Run();

    // Get references to each of the nodes that hold properties that we
    // care about
    JSBSim::FGPropertyManager* mgr = exec->GetPropertyManager();
    longitude_node_ = mgr->GetNode("position/long-gc-deg");
    latitude_node_ = mgr->GetNode("position/lat-gc-deg");
    altitude_node_ = mgr->GetNode("position/h-sl-ft");
    altitudeAGL_node_ = mgr->GetNode("position/h-agl-ft");

    roll_node_ = mgr->GetNode("attitude/roll-rad");
    pitch_node_ = mgr->GetNode("attitude/pitch-rad");
    yaw_node_ = mgr->GetNode("attitude/heading-true-rad");

    ap_aileron_cmd_node_ = mgr->GetNode("fcs/aileron-cmd-norm");
    ap_elevator_cmd_node_ = mgr->GetNode("fcs/elevator-cmd-norm");
    ap_rudder_cmd_node_ = mgr->GetNode("fcs/rudder-cmd-norm");
    ap_throttle_cmd_node_ = mgr->GetNode("fcs/throttle-cmd-norm");

    vel_north_node_ = mgr->GetNode("velocities/v-north-fps");
    vel_east_node_ = mgr->GetNode("velocities/v-east-fps");
    vel_down_node_ = mgr->GetNode("velocities/v-down-fps");

    u_vel_node_ = mgr->GetNode("velocities/u-fps");


    // angular velocity in ECEF frame
    p_node_ = mgr->GetNode("velocities/p-rad_sec");
    q_node_ = mgr->GetNode("velocities/q-rad_sec");
    r_node_ = mgr->GetNode("velocities/r-rad_sec");

    // acceleration at pilot location in body frame
    ax_pilot_node_ = mgr->GetNode("accelerations/a-pilot-x-ft_sec2");
    ay_pilot_node_ = mgr->GetNode("accelerations/a-pilot-y-ft_sec2");
    az_pilot_node_ = mgr->GetNode("accelerations/a-pilot-z-ft_sec2");


    // Save state
    parent_->projection()->Forward(latitude_node_->getDoubleValue(),
                                  longitude_node_->getDoubleValue(),
                                  altitude_node_->getDoubleValue() * feet2meters,
                                  state_->pos()(0), state_->pos()(1), state_->pos()(2));

    angles_from_jsbsim_.set_angle(ang::rad2deg(yaw_node_->getDoubleValue()));

    state_->quat().set(roll_node_->getDoubleValue(),
                      -pitch_node_->getDoubleValue(),
                      ang::deg2rad(angles_from_jsbsim_.angle()));

    state_->vel() << vel_east_node_->getDoubleValue() * feet2meters,
        vel_north_node_->getDoubleValue() * feet2meters,
        -vel_down_node_->getDoubleValue() * feet2meters;

    Eigen::Vector3d ang_vel_FLU(p_node_->getDoubleValue(),
                               -q_node_->getDoubleValue(),
                               -r_node_->getDoubleValue());
    state_->ang_vel() = state_->quat().rotate(ang_vel_FLU);

    Eigen::Vector3d a_FLU(ax_pilot_node_->getDoubleValue(),
                         -ay_pilot_node_->getDoubleValue(),
                         -az_pilot_node_->getDoubleValue());
    linear_accel_body_ = state_->quat().rotate(a_FLU);

    return true;
}

bool JSBSimControl::step(double time, double dt) {
    thrust_ = ba::clamp(vars_.input(thrust_idx_), -1.0, 1.0);
    delta_elevator_ = ba::clamp(vars_.input(elevator_idx_), -1.0, 1.0);
    delta_aileron_ = ba::clamp(vars_.input(aileron_idx_), -1.0, 1.0);
    delta_rudder_ = ba::clamp(vars_.input(rudder_idx_), -1.0, 1.0);

    ap_aileron_cmd_node_->setDoubleValue(delta_aileron_);
    ap_elevator_cmd_node_->setDoubleValue(delta_elevator_);
    ap_rudder_cmd_node_->setDoubleValue(delta_rudder_);
    ap_throttle_cmd_node_->setDoubleValue(thrust_);

    // double u_roll = u(0);
    // double u_pitch = u(1);
    // double u_yaw = u(2);
    //
    // // Roll stabilizer
    // ap_aileron_cmd_node_->setDoubleValue(u_roll);
    //
    // // Pitch stabilizer
    // if (time < 5) {
    //     ap_elevator_cmd_node_->setDoubleValue(u_pitch);
    // } else if (time < 7) {
    //     ap_elevator_cmd_node_->setDoubleValue(0.5);
    // } else {
    //     ap_elevator_cmd_node_->setDoubleValue(-0.5);
    // }

    // // Yaw stabilizer
    // ap_rudder_cmd_node_->setDoubleValue(u_yaw);

    exec->Setdt(dt);
    exec->Run();

    ///////////////////////////////////////////////////////////////////////////
    // Save state
    parent_->projection()->Forward(latitude_node_->getDoubleValue(),
                                  longitude_node_->getDoubleValue(),
                                  altitude_node_->getDoubleValue() * feet2meters,
                                  state_->pos()(0), state_->pos()(1), state_->pos()(2));

    angles_from_jsbsim_.set_angle(ang::rad2deg(yaw_node_->getDoubleValue()));

    state_->quat().set(roll_node_->getDoubleValue(),
                      -pitch_node_->getDoubleValue(),
                      ang::deg2rad(angles_from_jsbsim_.angle()));


    state_->vel() << vel_east_node_->getDoubleValue() * feet2meters,
        vel_north_node_->getDoubleValue() * feet2meters,
        -vel_down_node_->getDoubleValue() * feet2meters;


    Eigen::Vector3d ang_vel_FLU(p_node_->getDoubleValue(),
                               -q_node_->getDoubleValue(),
                               -r_node_->getDoubleValue());
    state_->ang_vel() = state_->quat().rotate(ang_vel_FLU);

    Eigen::Vector3d a_FLU(ax_pilot_node_->getDoubleValue() * feet2meters,
                         -ay_pilot_node_->getDoubleValue() * feet2meters,
                         -az_pilot_node_->getDoubleValue() * feet2meters);
    // TODO: jsbsim returns specific force, but need to populate this value with
    // acceleration. Need to make gravity not a hard-coded value or find a better
    // way to handle this.
    a_FLU = a_FLU + state_->quat().rotate_reverse(Eigen::Vector3d(0, 0, -9.81));
    linear_accel_body_ = a_FLU;

    Eigen::Vector3d a_ENU = state_->quat().rotate(a_FLU);



    // draw velocity
    if (drawVel_) {
        sc::ShapePtr shape(new sp::Shape());
        shape->set_type(sp::Shape::Line);
        shape->set_opacity(1.0);
        sc::add_point(shape, state_->pos() );
        Eigen::Vector3d color(255, 255, 0);
        sc::set(shape->mutable_color(), color[0], color[1], color[2]);
        sc::add_point(shape, state_->pos() + state_->vel()*drawVel_ );
        shapes_.push_back(shape);
    }


    // draw angular velocity
    if (drawAngVel_) {
        sc::ShapePtr shape(new sp::Shape());
        shape->set_type(sp::Shape::Line);
        shape->set_opacity(1.0);
        sc::add_point(shape, state_->pos() );
        Eigen::Vector3d color(0, 255, 255);
        sc::set(shape->mutable_color(), color[0], color[1], color[2]);
        sc::add_point(shape, state_->pos() + state_->ang_vel()*drawAngVel_ );
        shapes_.push_back(shape);
    }

    // draw acceleration
    if (drawAcc_) {
        sc::ShapePtr shape(new sp::Shape());
        shape->set_type(sp::Shape::Line);
        shape->set_opacity(1.0);
        sc::add_point(shape, state_->pos() );
        Eigen::Vector3d color(0, 0, 255);
        sc::set(shape->mutable_color(), color[0], color[1], color[2]);
        sc::add_point(shape, state_->pos() + a_ENU*drawAcc_ );
        shapes_.push_back(shape);
    }


#if 1
    JSBSim::FGPropertyManager* mgr = exec->GetPropertyManager();
    cout << "--------------------------------------------------------" << endl;
    cout << "  State information in JSBSImControl" << endl;
    cout << "--------------------------------------------------------" << endl;
    int prec = 5;
    cout << std::setprecision(prec) << "time: " << time << endl;
    cout << std::setprecision(prec) << "Altitude AGL: " << altitudeAGL_node_->getDoubleValue() * feet2meters << endl;
    cout << std::setprecision(prec) << "WOW[0]: " << mgr->GetNode("gear/unit/WOW")->getDoubleValue() << endl;
    cout << std::setprecision(prec) << "WOW[1]: " << mgr->GetNode("gear/unit[1]/WOW")->getDoubleValue() << endl;
    cout << std::setprecision(prec) << "WOW[2]: " << mgr->GetNode("gear/unit[2]/WOW")->getDoubleValue() << endl;
    cout << std::setprecision(prec) << "xAccel: " << linear_accel_body_(0) << endl;
    cout << std::setprecision(prec) << "yAccel: " << linear_accel_body_(1) << endl;
    cout << std::setprecision(prec) << "zAccel: " << linear_accel_body_(2) << endl;
    cout << std::setprecision(prec) << "aileron cmd: " << delta_aileron_ << endl;
    cout << std::setprecision(prec) << "elevator cmd: " << delta_elevator_ << endl;
    cout << std::setprecision(prec) << "rudder cmd: " << delta_rudder_ << endl;
    cout << std::setprecision(prec) << "thrust cmd: " << thrust_ << endl;
    cout << std::setprecision(prec) << "aileron jsb: " << mgr->GetNode("fcs/right-aileron-pos-norm")->getDoubleValue() << endl;
    cout << std::setprecision(prec) << "elevator jsb: " << mgr->GetNode("fcs/elevator-pos-norm")->getDoubleValue() << endl;
    cout << std::setprecision(prec) << "rudder jsb: " << mgr->GetNode("fcs/rudder-pos-norm")->getDoubleValue() << endl;
    cout << std::setprecision(prec) << "thrust jsb: " << mgr->GetNode("propulsion/engine/thrust-lbs")->getDoubleValue() << endl;
#endif

    return true;
}
} // namespace motion
} // namespace scrimmage
