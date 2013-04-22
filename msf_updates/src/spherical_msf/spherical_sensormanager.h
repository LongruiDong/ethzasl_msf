/*

Copyright (c) 2010, Stephan Weiss, ASL, ETH Zurich, Switzerland
You can contact the author at <stephan dot weiss at ieee dot org>
Copyright (c) 2012, Simon Lynen, ASL, ETH Zurich, Switzerland
You can contact the author at <slynen at ethz dot ch>
 Copyright (c) 2012, Markus Achtelik, ASL, ETH Zurich, Switzerland
 You can contact the author at <acmarkus at ethz dot ch>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
 * Neither the name of ETHZ-ASL nor the
names of its contributors may be used to endorse or promote products
derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL ETHZ-ASL BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */

#ifndef POSITION_MEASUREMENTMANAGER_H
#define POSITION_MEASUREMENTMANAGER_H

#include <ros/ros.h>

#include <msf_core/msf_core.h>
#include <msf_core/msf_sensormanagerROS.h>
#include "msf_statedef.hpp"
#include "spherical_sensorhandler.h"
#include "spherical_measurement.h"
#include <msf_updates/SphericalPositionSensorConfig.h>

namespace msf_spherical_position{

typedef msf_updates::SphericalPositionSensorConfig Config_T;
typedef dynamic_reconfigure::Server<Config_T> ReconfigureServer;
typedef boost::shared_ptr<ReconfigureServer> ReconfigureServerPtr;

class SensorManager : public msf_core::MSF_SensorManagerROS<msf_updates::EKFState>
{
  typedef AngleSensorHandler<AngleMeasurement, SensorManager> AngleSensorHandler_T;
  friend class AngleSensorHandler<AngleMeasurement, SensorManager>;
  typedef DistanceSensorHandler<DistanceMeasurement, SensorManager> DistanceSensorHandler_T;
  friend class DistanceSensorHandler<DistanceMeasurement, SensorManager>;
public:
  typedef msf_updates::EKFState EKFState_T;
  typedef EKFState_T::StateSequence_T StateSequence_T;
  typedef EKFState_T::StateDefinition_T StateDefinition_T;

  SensorManager(ros::NodeHandle pnh = ros::NodeHandle("~/spherical_position_sensor"))
  {
    angle_handler_.reset(new AngleSensorHandler_T(*this, "", "spherical_position_sensor"));
    addHandler(angle_handler_);
    distance_handler_.reset(new DistanceSensorHandler_T(*this, "", "spherical_position_sensor"));
    addHandler(distance_handler_);

    reconf_server_.reset(new ReconfigureServer(pnh));
    ReconfigureServer::CallbackType f = boost::bind(&SensorManager::config, this, _1, _2);
    reconf_server_->setCallback(f);
  }
  virtual ~SensorManager(){}

  virtual const Config_T& getcfg(){
    return config_;
  }

private:
  boost::shared_ptr<AngleSensorHandler_T> angle_handler_;
  boost::shared_ptr<DistanceSensorHandler_T> distance_handler_;

  Config_T config_;
  ReconfigureServerPtr reconf_server_; ///< dynamic reconfigure server

  /**
   * \brief dynamic reconfigure callback
   */
  virtual void config(Config_T &config, uint32_t level){
    config_ = config;
    angle_handler_->setNoises(config.angle_noise_meas);
    angle_handler_->setDelay(config.angle_delay);
    distance_handler_->setNoises(config.distance_noise_meas);
    distance_handler_->setDelay(config.distance_delay);

    if((level & msf_updates::SphericalPositionSensor_INIT_FILTER) && config.core_init_filter == true){
      init(1.0);
      config.core_init_filter = false;
    }
  }

  void init(double scale)
  {
    if(scale < 0.001){
      ROS_WARN_STREAM("init scale is "<<scale<<" correcting to 1");
      scale = 1;
    }

    Eigen::Matrix<double, 3, 1> p, v, b_w, b_a, g, w_m, a_m, p_ip, p_vc;
    Eigen::Quaternion<double> q;
    msf_core::MSF_Core<EKFState_T>::ErrorStateCov P;


    // init values
    g << 0, 0, 9.81;	        /// gravity
    b_w << 0,0,0;		/// bias gyroscopes
    b_a << 0,0,0;		/// bias accelerometer

    v << 0,0,0;			/// robot velocity (IMU centered)
    w_m << 0,0,0;		/// initial angular velocity
    a_m = g;			/// initial acceleration

    //set the initial yaw alignment of body to world (the frame in which the position sensor measures)
    double yawinit = config_.yaw_init / 180 * M_PI;
    Eigen::Quaterniond yawq(cos(yawinit / 2),0 ,0 , sin(yawinit / 2));
    yawq.normalize();

    q = yawq;

    P.setZero(); // error state covariance; if zero, a default initialization in msf_core is used

    // TODO: @georg get your measurements here and convert to xyz
    msf_core::Vector2 angles = angle_handler_->getAngleMeasurement();
    msf_core::Vector1 distance = distance_handler_->getDistanceMeasurement();
    // Check that angles(1) is theta and angles(2) is phi...
    p_vc(0,0) = distance(1) * sin(angles(1)) * cos(angles(2));
    p_vc(1,0) = distance(1) * sin(angles(1)) * sin(angles(2));
    p_vc(2,0) = distance(1) * cos(angles(1));

    ROS_INFO_STREAM("initial measurement pos:["<<p_vc.transpose()<<"] orientation: "<<STREAMQUAT(q));

    // check if we have already input from the measurement sensor
    if (p_vc.norm() == 0)
      ROS_WARN_STREAM("No measurements received yet to initialize position - using [0 0 0]");

    ros::NodeHandle pnh("~");
    pnh.param("position_sensor/init/p_ip/x", p_ip[0], 0.0);
    pnh.param("position_sensor/init/p_ip/y", p_ip[1], 0.0);
    pnh.param("position_sensor/init/p_ip/z", p_ip[2], 0.0);

    // calculate initial attitude and position based on sensor measurements
    p = p_vc - q.toRotationMatrix() * p_ip;

    //prepare init "measurement"
    boost::shared_ptr<msf_core::MSF_InitMeasurement<EKFState_T> > meas(new msf_core::MSF_InitMeasurement<EKFState_T>(true)); //hand over that we will also set the sensor readings

    meas->setStateInitValue<StateDefinition_T::p>(p);
    meas->setStateInitValue<StateDefinition_T::v>(v);
    meas->setStateInitValue<StateDefinition_T::q>(q);
    meas->setStateInitValue<StateDefinition_T::b_w>(b_w);
    meas->setStateInitValue<StateDefinition_T::b_a>(b_a);
    meas->setStateInitValue<StateDefinition_T::p_ip>(p_ip);

    setP(meas->get_P()); //call my set P function
    meas->get_w_m() = w_m;
    meas->get_a_m() = a_m;
    meas->time = ros::Time::now().toSec();

    // call initialization in core
    msf_core_->init(meas);
  }

  //prior to this call, all states are initialized to zero/identity
  virtual void resetState(EKFState_T& state){
    UNUSED(state);
  }
  virtual void initState(EKFState_T& state){
    UNUSED(state);
  }

  virtual void calculateQAuxiliaryStates(EKFState_T& state, double dt){
    const msf_core::Vector3 npipv = msf_core::Vector3::Constant(config_.noise_p_ip);

    //compute the blockwise Q values and store them with the states,
    //these then get copied by the core to the correct places in Qd
    state.getQBlock<StateDefinition_T::p_ip>() = (dt * npipv.cwiseProduct(npipv)).asDiagonal();
  }

  virtual void setP(Eigen::Matrix<double, EKFState_T::nErrorStatesAtCompileTime, EKFState_T::nErrorStatesAtCompileTime>& P){
    UNUSED(P);
    //nothing, we only use the simulated cov for the core plus diagonal for the rest
  }

  virtual void augmentCorrectionVector(Eigen::Matrix<double, EKFState_T::nErrorStatesAtCompileTime,1>& correction){
    UNUSED(correction);
  }

  virtual void sanityCheckCorrection(EKFState_T& delaystate, const EKFState_T& buffstate,
                                     Eigen::Matrix<double, EKFState_T::nErrorStatesAtCompileTime,1>& correction){
    UNUSED(delaystate);
    UNUSED(buffstate);
    UNUSED(correction);
  }

};

}
#endif /* POSITION_MEASUREMENTMANAGER_H */
