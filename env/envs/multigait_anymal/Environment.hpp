//----------------------------//
// This file is part of RaiSim//
// Copyright 2020, RaiSim Tech//
//----------------------------//

#pragma once

#include <cstdlib>
#include <cmath>
#include <set>
#include "../../RaisimGymEnv.hpp"

namespace raisim {

class ENVIRONMENT : public RaisimGymEnv {

 public:

  explicit ENVIRONMENT(const std::string& resourceDir, const Yaml::Node& cfg, bool visualizable) :
      RaisimGymEnv(resourceDir, cfg), visualizable_(visualizable) {

    /// add objects
    // anymal_ = world_->addArticulatedSystem(resourceDir_+"/anymal/urdf/anymal.urdf");
    anymal_ = world_->addArticulatedSystem(resourceDir_+"/laikago/laikago.urdf");
    anymal_->setName("anymal");
    anymal_->setControlMode(raisim::ControlMode::PD_PLUS_FEEDFORWARD_TORQUE);
    world_->addGround();

    /// get robot data
    gcDim_ = anymal_->getGeneralizedCoordinateDim();  // 15 (w/ fixed HAA joints)
    gvDim_ = anymal_->getDOF();  // 14 (w/ fixed HAA joints)
    nJoints_ = gvDim_ - 6;  // 8 (w/ fixed HAA joints)

    // std::cout << gcDim_ << std::endl;
    // std::cout << gvDim_ << std::endl;
    // std::cout << nJoints_ << std::endl;

    /// initialize containers
    gc_.setZero(gcDim_); gc_init_.setZero(gcDim_);
    gv_.setZero(gvDim_); gv_init_.setZero(gvDim_);
    pTarget_.setZero(gcDim_); vTarget_.setZero(gvDim_); pTarget12_.setZero(nJoints_);

    /// desired velocity
    desired_velocity = cfg["velocity"].As<double>();

    /// reward constant
    reward_torque_coeff = cfg["reward"]["torque"]["coeff"].As<double>();
    reward_velocity_coeff = cfg["reward"]["forwardVel_difference"]["coeff"].As<double>();
    reward_GRF_coeff = cfg["reward"]["GRF_entropy"]["coeff"].As<double>();

    /// contact foot index
    contact_foot_idx.setZero(4);
    contact_foot_idx[0] = anymal_->getBodyIdx("FR_calf");
    contact_foot_idx[1] = anymal_->getBodyIdx("FL_calf");
    contact_foot_idx[2] = anymal_->getBodyIdx("RR_calf");
    contact_foot_idx[3] = anymal_->getBodyIdx("RL_calf");

    /// this is nominal configuration of anymal
    // gc_init_ << 0, 0, 0.50, 1.0, 0.0, 0.0, 0.0, 0.4, -0.8, 0.4, -0.8, -0.4, 0.8, -0.4, 0.8;
    // gc_init_ << 0, 0, 0.50, 1.0, 0.0, 0.0, 0.0, 0.03, 0.4, -0.8, -0.03, 0.4, -0.8, 0.03, -0.4, 0.8, -0.03, -0.4, 0.8;

    // initialize for laikago
    gc_init_ << 0, 0, 0.46, 1, 0.0, 0.0, 0.0, 0.5, -1, 0.5, -1, 0.5, -1, 0.5, -1;

    /// set pd gains
    Eigen::VectorXd jointPgain(gvDim_), jointDgain(gvDim_);
    jointPgain.setZero(); jointPgain.tail(nJoints_).setConstant(40.0); //20.0
    jointDgain.setZero(); jointDgain.tail(nJoints_).setConstant(1.0); //0.2
    anymal_->setPdGains(jointPgain, jointDgain);
    anymal_->setGeneralizedForce(Eigen::VectorXd::Zero(gvDim_));

    /// MUST BE DONE FOR ALL ENVIRONMENTS
    obDim_ = 26;
    actionDim_ = nJoints_; actionMean_.setZero(actionDim_); actionStd_.setZero(actionDim_);
    obDouble_.setZero(obDim_);

    /// action scaling
    actionMean_ = gc_init_.tail(nJoints_);
    actionStd_.setConstant(0.3);

    /// Reward coefficients
    rewards_.initializeFromConfigurationFile (cfg["reward"]);

    /// indices of links that should not make contact with ground
    footIndices_.insert(anymal_->getBodyIdx("FR_calf")); // 2
    footIndices_.insert(anymal_->getBodyIdx("FL_calf")); // 4
    footIndices_.insert(anymal_->getBodyIdx("RR_calf")); // 6
    footIndices_.insert(anymal_->getBodyIdx("RL_calf")); // 8
    // footIndices_.insert(anymal_->getBodyIdx("trunk"));

    /// visualize if it is the first environment
    if (visualizable_) {
      server_ = std::make_unique<raisim::RaisimServer>(world_.get());
      server_->launchServer();
      server_->focusOn(anymal_);
    }
  }

  void init() final { }

  void reset() final {
    anymal_->setState(gc_init_, gv_init_);
    updateObservation();
  }

  float step(const Eigen::Ref<EigenVec>& action) final {
    /// action scaling
    pTarget12_ = action.cast<double>();
    pTarget12_ = pTarget12_.cwiseProduct(actionStd_);
    pTarget12_ += actionMean_;
    pTarget_.tail(nJoints_) = pTarget12_;

    anymal_->setPdTarget(pTarget_, vTarget_);

    for(int i=0; i< int(control_dt_ / simulation_dt_ + 1e-10); i++){
      if(server_) server_->lockVisualizationServerMutex();
      world_->integrate();
      if(server_) server_->unlockVisualizationServerMutex();
    }

    updateObservation();

    rewards_.record("torque", anymal_->getGeneralizedForce().squaredNorm());
    // velocity = std::pow(std::pow(bodyLinearVel_[0], 2) + std::pow(bodyLinearVel_[1], 2), 0.5);
    // velocity = std::min(4.0, bodyLinearVel_[0]);
    rewards_.record("forwardVel_difference", std::exp(- std::abs(bodyLinearVel_[0] - desired_velocity)));
    // rewards_.record("forwardVel", std::min(4.0, bodyLinearVel_[0]));
    rewards_.record("GRF_entropy", GRF_entropy);

    return rewards_.sum();
  }

  void reward_logging(Eigen::Ref<EigenVec> rewards) final {
    reward_log.setZero(3);
    reward_log[0] = anymal_->getGeneralizedForce().squaredNorm() * reward_torque_coeff;
    reward_log[1] = - std::abs(bodyLinearVel_[0] - desired_velocity) * reward_velocity_coeff;
    reward_log[2] = GRF_entropy * reward_GRF_coeff;
    rewards = reward_log.cast<float>();
  }

  void updateObservation() {
    anymal_->getState(gc_, gv_);

    // Eigen::IOFormat CommaInitFmt(Eigen::StreamPrecision, Eigen::DontAlignCols, ", ", ", ", "", "", " << ", ";");
    // std::cout << gc_.format(CommaInitFmt) << std::endl;
    // std::cout << gv_.format(CommaInitFmt) << std::endl;

    raisim::Vec<4> quat;
    raisim::Mat<3,3> rot;
    quat[0] = gc_[3]; quat[1] = gc_[4]; quat[2] = gc_[5]; quat[3] = gc_[6];
    raisim::quatToRotMat(quat, rot);
    bodyLinearVel_ = rot.e().transpose() * gv_.segment(0, 3);
    bodyAngularVel_ = rot.e().transpose() * gv_.segment(3, 3);

    obDouble_ << gc_[2], /// body height // dim=1
        rot.e().row(2).transpose(), /// body orientation // dim=3
        gc_.tail(8), /// joint angles // dim=8 (w/ HAA joint fixed)
        bodyLinearVel_, bodyAngularVel_, /// body linear&angular velocity // dim=6 (3 + 3)
        gv_.tail(8); /// joint velocity // dim=8 (w/ HAA joint fixed)
    
    /// z axis contact impulse for each feet (= perpendicular GRF * dt)
    total_contact_impulse.setZero(4);

    for(auto& contact: anymal_->getContacts()) {
      if (contact.skip()) continue; /// if the contact is internal, one contact point is set to 'skip'
      
      single_contact_impulse = contact.getContactFrame().e().transpose() * contact.getImpulse()->e();

      for (int i; i<4; i++) {
        if (contact_foot_idx[i] == contact.getlocalBodyIndex()) {
          total_contact_impulse[i] = single_contact_impulse[-1];
          continue;
        }
      }

      /// compute perpendicular GRF entropy
      total_contact_impulse.normalize();
      total_contact_impulse = total_contact_impulse.array() + 1e-6;
      GRF_entropy = - (total_contact_impulse.array() * total_contact_impulse.array().log()).sum();

      // if ( footIndex == contact.getlocalBodyIndex() ) {
        // std::cout<<"Contact impudelse in the world frame: "<< contact.getContactFrame().e().transpose() * contact.getImpulse()->e()<<std::endl;
      // }
    }

  }

  void observe(Eigen::Ref<EigenVec> ob) final {
    /// convert it to float
    ob = obDouble_.cast<float>();
  }

  bool isTerminalState(float& terminalReward) final {
    terminalReward = float(terminalRewardCoeff_);

    /// if the contact body is not feet
    for(auto& contact: anymal_->getContacts())
      if(footIndices_.find(contact.getlocalBodyIndex()) == footIndices_.end())
        return true;

    terminalReward = 0.f;
    return false;
  }

 private:
  int gcDim_, gvDim_, nJoints_;
  bool visualizable_ = false;
  raisim::ArticulatedSystem* anymal_;
  Eigen::VectorXd gc_init_, gv_init_, gc_, gv_, pTarget_, pTarget12_, vTarget_;
  double terminalRewardCoeff_ = -10., velocity, desired_velocity, reward_torque_coeff, reward_velocity_coeff, reward_GRF_coeff, GRF_entropy;
  Eigen::VectorXd actionMean_, actionStd_, obDouble_, reward_log, contact_foot_idx, single_contact_impulse, total_contact_impulse;
  Eigen::Vector3d bodyLinearVel_, bodyAngularVel_;
  std::set<size_t> footIndices_;
};
}

