// Copyright (c) 2021 Franka Emika GmbH
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cassert>
#include <mutex>

#include <franka/control_tools.h>
#include <franka/rate_limiting.h>
#include <research_interface/robot/rbk_types.h>
#include <rclcpp/logging.hpp>

#include "franka_hardware/robot.hpp"

namespace franka_hardware {

Robot::Robot(const std::string& robot_ip, const rclcpp::Logger& logger) {
  franka::RealtimeConfig rt_config = franka::RealtimeConfig::kEnforce;
  if (!franka::hasRealtimeKernel()) {
    rt_config = franka::RealtimeConfig::kIgnore;
    RCLCPP_WARN(
        logger,
        "You are not using a real-time kernel. Using a real-time kernel is strongly "
        "recommended! Information about how to set up a real-time kernel can be found here: "
        "https://frankaemika.github.io/docs/"
        "installation_linux.html#setting-up-the-real-time-kernel");
  }
  robot_ = std::make_unique<franka::Robot>(robot_ip, rt_config);
  model_ = std::make_unique<franka::Model>(robot_->loadModel());
  franka_hardware_model_ = std::make_unique<Model>(model_.get());
}

Robot::~Robot() {
  stopRobot();
}

franka::RobotState Robot::readOnce() {
  std::lock_guard<std::mutex> lock(control_mutex_);
  if (!isControlLoopActive()) {
    current_state_ = robot_->readOnce();
    return current_state_;
  } else {
    return readOnceActiveControl();
  }
}

void Robot::stopRobot() {
  if (isControlLoopActive()) {
    effort_interface_active_ = false;
    joint_velocity_interface_active_ = false;
    cartesian_velocity_interface_active_ = false;
    active_control_.reset();
  }
}

void Robot::writeOnce(const std::array<double, 7>& joint_commands) {
  if (effort_interface_active_) {
    writeOnceEfforts(joint_commands);
  } else if (joint_velocity_interface_active_) {
    writeOnceJointVelocities(joint_commands);
  }
}

void Robot::writeOnceEfforts(const std::array<double, 7>& efforts) {
  std::lock_guard<std::mutex> lock(control_mutex_);

  auto torque_command = franka::Torques(efforts);
  torque_command.tau_J =
      franka::limitRate(franka::kMaxTorqueRate, torque_command.tau_J, last_desired_torque_);
  last_desired_torque_ = torque_command.tau_J;

  active_control_->writeOnce(torque_command);
}

void Robot::writeOnceJointVelocities(const std::array<double, 7>& velocities) {
  std::lock_guard<std::mutex> lock(control_mutex_);

  auto velocity_command = franka::JointVelocities(velocities);

  // If you are experiencing issues with robot error. You can try activating the rate limiter.
  // Rate limiter is default deactivated.
  if (velocity_command_rate_limit_active_) {
    velocity_command.dq = franka::limitRate(
        franka::computeUpperLimitsJointVelocity(current_state_.q_d),
        franka::computeLowerLimitsJointVelocity(current_state_.q_d), franka::kMaxJointAcceleration,
        franka::kMaxJointJerk, velocity_command.dq, current_state_.dq_d, current_state_.ddq_d);
  }

  active_control_->writeOnce(velocity_command);
}

void Robot::writeOnce(const std::array<double, 6>& cartesian_velocities) {
  std::lock_guard<std::mutex> lock(control_mutex_);

  auto velocity_command = franka::CartesianVelocities(cartesian_velocities);

  if (cartesian_velocity_low_pass_filter_active) {
    for (size_t i = 0; i < 6; i++) {
      velocity_command.O_dP_EE[i] =
          franka::lowpassFilter(franka::kDeltaT, velocity_command.O_dP_EE[i],
                                current_state_.O_dP_EE_c[i], low_pass_filter_cut_off_freq);
    }
  }

  // If you are experiencing issues with robot error. You can try activating the rate
  // limiter. Rate limiter is default deactivated (cartesian_velocity_command_rate_limit_active_)
  if (cartesian_velocity_command_rate_limit_active_) {
    velocity_command.O_dP_EE = franka::limitRate(
        franka::kMaxTranslationalVelocity, franka::kMaxTranslationalAcceleration,
        franka::kMaxTranslationalJerk, franka::kMaxRotationalVelocity,
        franka::kMaxRotationalAcceleration, franka::kMaxRotationalJerk, velocity_command.O_dP_EE,
        current_state_.O_dP_EE_c, current_state_.O_ddP_EE_c);
  }
  franka::checkFinite(velocity_command.O_dP_EE);

  active_control_->writeOnce(velocity_command);
}

void Robot::writeOnce(const std::array<double, 6>& cartesian_velocities,
                      const std::array<double, 2>& elbow_command) {
  std::lock_guard<std::mutex> lock(control_mutex_);

  // check if the elbow is zeros. If full zeros call writeOnce(cartesian_velocities)
  // std::cout << "WRITING CART VEL WITH ELBOW" << std::endl;
  // writeOnce(cartesian_velocities);
  auto velocity_command = franka::CartesianVelocities(cartesian_velocities, elbow_command);

  if (cartesian_velocity_low_pass_filter_active) {
    for (size_t i = 0; i < 6; i++) {
      velocity_command.O_dP_EE[i] =
          franka::lowpassFilter(franka::kDeltaT, velocity_command.O_dP_EE[i],
                                current_state_.O_dP_EE_c[i], low_pass_filter_cut_off_freq);
    }
  }

  // If you are experiencing issues with robot error. You can try activating the rate
  // limiter. Rate limiter is default deactivated (cartesian_velocity_command_rate_limit_active_)
  if (cartesian_velocity_command_rate_limit_active_) {
    velocity_command.O_dP_EE = franka::limitRate(
        franka::kMaxTranslationalVelocity, franka::kMaxTranslationalAcceleration,
        franka::kMaxTranslationalJerk, franka::kMaxRotationalVelocity,
        franka::kMaxRotationalAcceleration, franka::kMaxRotationalJerk, velocity_command.O_dP_EE,
        current_state_.O_dP_EE_c, current_state_.O_ddP_EE_c);
  }
  franka::checkFinite(velocity_command.O_dP_EE);

  active_control_->writeOnce(velocity_command);
}

franka::RobotState Robot::readOnceActiveControl() {
  // When controller is active use active control to read the robot state
  const auto [current_state_, _] = active_control_->readOnce();
  return current_state_;
}

franka_hardware::Model* Robot::getModel() {
  return franka_hardware_model_.get();
}

void Robot::initializeTorqueInterface() {
  active_control_ = robot_->startTorqueControl();
  effort_interface_active_ = true;
}

void Robot::initializeJointVelocityInterface() {
  active_control_ = robot_->startJointVelocityControl(
      research_interface::robot::Move::ControllerMode::kJointImpedance);
  joint_velocity_interface_active_ = true;
}

void Robot::initializeCartesianVelocityInterface() {
  active_control_ = robot_->startCartesianVelocityControl(
      research_interface::robot::Move::ControllerMode::kJointImpedance);
  cartesian_velocity_interface_active_ = true;
}

bool Robot::isControlLoopActive() {
  return joint_velocity_interface_active_ || effort_interface_active_ ||
         cartesian_velocity_interface_active_;
}

void Robot::setJointStiffness(const franka_msgs::srv::SetJointStiffness::Request::SharedPtr& req) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  std::array<double, 7> joint_stiffness{};
  std::copy(req->joint_stiffness.cbegin(), req->joint_stiffness.cend(), joint_stiffness.begin());
  robot_->setJointImpedance(joint_stiffness);
}

void Robot::setCartesianStiffness(
    const franka_msgs::srv::SetCartesianStiffness::Request::SharedPtr& req) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  std::array<double, 6> cartesian_stiffness{};
  std::copy(req->cartesian_stiffness.cbegin(), req->cartesian_stiffness.cend(),
            cartesian_stiffness.begin());
  robot_->setCartesianImpedance(cartesian_stiffness);
}

void Robot::setLoad(const franka_msgs::srv::SetLoad::Request::SharedPtr& req) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  double mass(req->mass);
  std::array<double, 3> center_of_mass{};  // NOLINT [readability-identifier-naming]
  std::copy(req->center_of_mass.cbegin(), req->center_of_mass.cend(), center_of_mass.begin());
  std::array<double, 9> load_inertia{};
  std::copy(req->load_inertia.cbegin(), req->load_inertia.cend(), load_inertia.begin());

  robot_->setLoad(mass, center_of_mass, load_inertia);
}

void Robot::setTCPFrame(const franka_msgs::srv::SetTCPFrame::Request::SharedPtr& req) {
  std::lock_guard<std::mutex> lock(write_mutex_);

  std::array<double, 16> transformation{};  // NOLINT [readability-identifier-naming]
  std::copy(req->transformation.cbegin(), req->transformation.cend(), transformation.begin());
  robot_->setEE(transformation);
}

void Robot::setStiffnessFrame(const franka_msgs::srv::SetStiffnessFrame::Request::SharedPtr& req) {
  std::lock_guard<std::mutex> lock(write_mutex_);

  std::array<double, 16> transformation{};
  std::copy(req->transformation.cbegin(), req->transformation.cend(), transformation.begin());
  robot_->setK(transformation);
}

void Robot::setForceTorqueCollisionBehavior(
    const franka_msgs::srv::SetForceTorqueCollisionBehavior::Request::SharedPtr& req) {
  std::lock_guard<std::mutex> lock(write_mutex_);

  std::array<double, 7> lower_torque_thresholds_nominal{};
  std::copy(req->lower_torque_thresholds_nominal.cbegin(),
            req->lower_torque_thresholds_nominal.cend(), lower_torque_thresholds_nominal.begin());
  std::array<double, 7> upper_torque_thresholds_nominal{};
  std::copy(req->upper_torque_thresholds_nominal.cbegin(),
            req->upper_torque_thresholds_nominal.cend(), upper_torque_thresholds_nominal.begin());
  std::array<double, 6> lower_force_thresholds_nominal{};
  std::copy(req->lower_force_thresholds_nominal.cbegin(),
            req->lower_force_thresholds_nominal.cend(), lower_force_thresholds_nominal.begin());
  std::array<double, 6> upper_force_thresholds_nominal{};
  std::copy(req->upper_force_thresholds_nominal.cbegin(),
            req->upper_force_thresholds_nominal.cend(), upper_force_thresholds_nominal.begin());

  robot_->setCollisionBehavior(lower_torque_thresholds_nominal, upper_torque_thresholds_nominal,
                               lower_force_thresholds_nominal, upper_force_thresholds_nominal);
}

void Robot::setFullCollisionBehavior(
    const franka_msgs::srv::SetFullCollisionBehavior::Request::SharedPtr& req) {
  std::lock_guard<std::mutex> lock(write_mutex_);

  std::array<double, 7> lower_torque_thresholds_acceleration{};
  std::copy(req->lower_torque_thresholds_acceleration.cbegin(),
            req->lower_torque_thresholds_acceleration.cend(),
            lower_torque_thresholds_acceleration.begin());
  std::array<double, 7> upper_torque_thresholds_acceleration{};
  std::copy(req->upper_torque_thresholds_acceleration.cbegin(),
            req->upper_torque_thresholds_acceleration.cend(),
            upper_torque_thresholds_acceleration.begin());
  std::array<double, 7> lower_torque_thresholds_nominal{};
  std::copy(req->lower_torque_thresholds_nominal.cbegin(),
            req->lower_torque_thresholds_nominal.cend(), lower_torque_thresholds_nominal.begin());
  std::array<double, 7> upper_torque_thresholds_nominal{};
  std::copy(req->upper_torque_thresholds_nominal.cbegin(),
            req->upper_torque_thresholds_nominal.cend(), upper_torque_thresholds_nominal.begin());
  std::array<double, 6> lower_force_thresholds_acceleration{};
  std::copy(req->lower_force_thresholds_acceleration.cbegin(),
            req->lower_force_thresholds_acceleration.cend(),
            lower_force_thresholds_acceleration.begin());
  std::array<double, 6> upper_force_thresholds_acceleration{};
  std::copy(req->upper_force_thresholds_acceleration.cbegin(),
            req->upper_force_thresholds_acceleration.cend(),
            upper_force_thresholds_acceleration.begin());
  std::array<double, 6> lower_force_thresholds_nominal{};
  std::copy(req->lower_force_thresholds_nominal.cbegin(),
            req->lower_force_thresholds_nominal.cend(), lower_force_thresholds_nominal.begin());
  std::array<double, 6> upper_force_thresholds_nominal{};
  std::copy(req->upper_force_thresholds_nominal.cbegin(),
            req->upper_force_thresholds_nominal.cend(), upper_force_thresholds_nominal.begin());
  robot_->setCollisionBehavior(
      lower_torque_thresholds_acceleration, upper_torque_thresholds_acceleration,
      lower_torque_thresholds_nominal, upper_torque_thresholds_nominal,
      lower_force_thresholds_acceleration, upper_force_thresholds_acceleration,
      lower_force_thresholds_nominal, upper_force_thresholds_nominal);
}

}  // namespace franka_hardware
