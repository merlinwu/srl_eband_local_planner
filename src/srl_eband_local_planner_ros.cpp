/*********************************************************************
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2015, University of Freiburg
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Luigi Palmieri
 *********************************************************************/

#include <srl_eband_local_planner/srl_eband_local_planner_ros.h>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/thread/thread.hpp>
// pluginlib macros (defines, ...)
#include <pluginlib/class_list_macros.h>

// abstract class from which our plugin inherits
#include <nav_core/base_local_planner.h>

using namespace spencer_control_msgs;
// register this planner as a BaseGlobalPlanner plugin
// (see http://www.ros.org/wiki/pluginlib/Tutorials/Writing%20and%20Using%20a%20Simple%20Plugin)
// PLUGINLIB_DECLARE_CLASS(srl_eband_local_planner, SrlEBandPlannerROS, srl_eband_local_planner::SrlEBandPlannerROS, nav_core::BaseLocalPlanner)
PLUGINLIB_EXPORT_CLASS(srl_eband_local_planner::SrlEBandPlannerROS, nav_core::BaseLocalPlanner);

  namespace srl_eband_local_planner{

    SrlEBandPlannerROS::SrlEBandPlannerROS() : costmap_ros_(NULL), tf_(NULL), initialized_(false) {}


    SrlEBandPlannerROS::SrlEBandPlannerROS(std::string name, tf::TransformListener* tf, costmap_2d::Costmap2DROS* costmap_ros)
      : costmap_ros_(NULL), tf_(NULL), initialized_(false)
    {
      // initialize planner
      number_tentative_setting_band_ = 10;
      collision_error_rear_ = false;
      collision_error_front_ = false;
      collision_warning_rear_ = false;
      collision_warning_front_ = false;
      check_costmap_layers_ = false;
      dir_planning_ = -1; // starting Backward
      initialize(name, tf, costmap_ros);
    }


    SrlEBandPlannerROS::~SrlEBandPlannerROS() {}


    void SrlEBandPlannerROS::initialize(std::string name, tf::TransformListener* tf, costmap_2d::Costmap2DROS* costmap_ros)
    {
      // check if the plugin is already initialized
      if(!initialized_)
      {
        // copy adress of costmap and Transform Listener (handed over from move_base)
        costmap_ros_ = costmap_ros;
        tf_ = tf;
        optimize_band_ = true;

        // create Node Handle with name of plugin (as used in move_base for loading)
        ros::NodeHandle pn("~/" + name);

        // read parameters from parameter server
        // get tolerances for "Target reached"
        pn.param("yaw_goal_tolerance", yaw_goal_tolerance_, 0.05);
        pn.param("xy_goal_tolerance", xy_goal_tolerance_, 0.1);

        // set lower bound for velocity -> if velocity in this region stop! (to avoid limit-cycles or lock)
        pn.param("rot_stopped_vel", rot_stopped_vel_, 1e-2);
        pn.param("trans_stopped_vel", trans_stopped_vel_, 1e-2);

        // advertise topics (adapted global plan and predicted local trajectory)
        g_plan_pub_ = pn.advertise<nav_msgs::Path>("global_plan", 1);
        l_plan_pub_ = pn.advertise<nav_msgs::Path>("local_plan", 1);

        sub_current_measured_velocity_ = pn.subscribe("/spencer/control/measured_velocity", 5, &SrlEBandPlannerROS::readVelocityCB, this);
        frontLaserCollisionStatus_listener_ = pn.subscribe("/spencer/control/collision/aggregated_front", 1, &SrlEBandPlannerROS::checkFrontLaserCollisionStatus, this);
        rearLaserCollisionStatus_listener_  = pn.subscribe("/spencer/control/collision/aggregated_rear", 1, &SrlEBandPlannerROS::checkRearLaserCollisionStatus, this);
        sub_current_driving_direction_ = pn.subscribe("/spencer/nav/current_driving_direction", 1, &SrlEBandPlannerROS::SetDrivingDirection, this);

        pn.param("check_costmap_layers", check_costmap_layers_, false);



        // subscribe to topics (to get odometry information, we need to get a handle to the topic in the global namespace)
        ros::NodeHandle gn;
        odom_sub_ = gn.subscribe<nav_msgs::Odometry>("odom", 1, boost::bind(&SrlEBandPlannerROS::odomCallback, this, _1));


        // create the actual planner that we'll use. Pass Name of plugin and pointer to global costmap to it.
        // (configuration is done via parameter server)
        eband_ = boost::shared_ptr<SrlEBandPlanner>(new SrlEBandPlanner(name, costmap_ros_));

        // create the according controller
        eband_trj_ctrl_ = boost::shared_ptr<SrlEBandTrajectoryCtrl>(new SrlEBandTrajectoryCtrl(name, costmap_ros_, tf_));

        // create object for visualization
        eband_visual_ = boost::shared_ptr<SrlEBandVisualization>(new SrlEBandVisualization);

        // pass visualization object to elastic band
        eband_->setVisualization(eband_visual_);

        // pass visualization object to controller
        eband_trj_ctrl_->setVisualization(eband_visual_);

        // initialize visualization - set node handle and pointer to costmap
        eband_visual_->initialize(pn, costmap_ros);


        // set initialized flag
        initialized_ = true;

        dir_planning_ = -1;

        dr_server_ = new dynamic_reconfigure::Server<srl_eband_local_planner::srlEBandLocalPlannerConfig>(pn);

        dynamic_reconfigure::Server<srl_eband_local_planner::srlEBandLocalPlannerConfig>::CallbackType cb = boost::bind(
          &SrlEBandPlannerROS::callbackDynamicReconfigure, this, _1, _2);

        dr_server_->setCallback(cb);

        this->costmap_layers_handler_ = new costmapLayersDynRecHandler(pn);

        initial_social_range_ = costmap_layers_handler_->getSocialLayerMaxRange();

        service_enable_social_layer_ =  pn.advertiseService("enable_social_layer", &SrlEBandPlannerROS::enableSocialLayer, this);

        service_enable_obstacle_layer_ =  pn.advertiseService("enable_obstacle_layer", &SrlEBandPlannerROS::enableObstacleLayer, this);

        enable_social_layer_ = true;

        enable_obstacle_layer_ = true;


        // this is only here to make this process visible in the rxlogger right from the start
        ROS_DEBUG("Elastic Band plugin initialized.");

      }
      else
      {
        ROS_WARN("This planner has already been initialized, doing nothing.");
      }
    }


    /// ========================================================================================
    /// SetDrivingDirection
    /// Set The correct Driving Direction
    /// ========================================================================================
    bool SrlEBandPlannerROS::enableObstacleLayer(srl_eband_local_planner::EnableObstacleLayer::Request  &req,
             srl_eband_local_planner::EnableObstacleLayer::Response &res)
    {
        enable_obstacle_layer_ = req.enable;
        ROS_DEBUG("Service enableObstacleLayer called %d", enable_obstacle_layer_);
        ROS_DEBUG("Service enableObstacleLayer called, ending");
        res.enabled = enable_obstacle_layer_;
        return true;
    }





    /// ========================================================================================
    /// SetDrivingDirection
    /// Set The correct Driving Direction
    /// ========================================================================================
    bool SrlEBandPlannerROS::enableSocialLayer(srl_eband_local_planner::EnableSocialLayer::Request  &req,
             srl_eband_local_planner::EnableSocialLayer::Response &res)
    {
        enable_social_layer_ = req.enable;
        ROS_DEBUG("Service enableSocialLayer called %d",
                    enable_social_layer_);
        ROS_DEBUG("Service enableSocialLayer call finished");
        res.enabled = enable_social_layer_;
        return true;
    }

    /// ========================================================================
    /// setCostmapsLayers
    /// ========================================================================
    bool SrlEBandPlannerROS::setCostmapsLayers(){

        ROS_DEBUG("Set costmap layers, %d %d", enable_social_layer_,
         enable_obstacle_layer_);


        if(costmap_layers_handler_->isObstacleLayerEnabled() &&
              !enable_obstacle_layer_){
          if(!costmap_layers_handler_->enableObstacleLayer(false))
          {
            ROS_ERROR("Could not plan due to issues during disabling of the Obstacle layer");
            costmap_layers_handler_->enableObstacleLayer(true);
            return false;
          }
          ROS_INFO("Disabling Obstacle layer in srl_eband_local_planner");
        }


        if(!costmap_layers_handler_->isObstacleLayerEnabled() &&
              enable_obstacle_layer_)
        {
          if(!costmap_layers_handler_->enableObstacleLayer(true))
          {
            ROS_ERROR("Could not plan due to issues during enabling of the Obstacle layer");
            costmap_layers_handler_->enableObstacleLayer(false);
            return false;
          }
          ROS_INFO("Enabling Obstacle layer in srl_eband_local_planner");
        }


        if(costmap_layers_handler_->isSocialLayerEnabled() &&
              !enable_social_layer_){
          if(!costmap_layers_handler_->enableSocialLayer(false))
          {
            ROS_ERROR("Could not plan due to issues during disabling of the Social layer");
            costmap_layers_handler_->enableSocialLayer(true);
            return false;
          }
          ROS_INFO("Disabling Social layer in srl_eband_local_planner");
        }

        if(!costmap_layers_handler_->isSocialLayerEnabled() &&
              enable_social_layer_)
        {
          if(!costmap_layers_handler_->enableSocialLayer(true))
          {
            ROS_ERROR("Could not plan due to issues during enabling of the Social layer");
            costmap_layers_handler_->enableSocialLayer(false);
            return false;
          }
          ROS_INFO("Enabling Social layer in srl_eband_local_planner");
        }

        ROS_DEBUG("Setting costmap layers ended");

        return true;
    }


    /// ========================================================================================
    /// SetDrivingDirection
    /// Set The correct Driving Direction
    /// ========================================================================================
    void SrlEBandPlannerROS::SetDrivingDirection(const std_msgs::Bool::ConstPtr& msg){
      bool forward = false;
      forward = msg->data;
      if(!forward){
        ROS_DEBUG_THROTTLE(5, "Backward direction set");
        dir_planning_=-1*fabs(dir_planning_);
      }else
      {
        ROS_DEBUG_THROTTLE(5, "Forward  direction set");
        dir_planning_=1*fabs(dir_planning_);
      }
      return;
    }

    /// ==================================================================================
    /// checkFrontLaserCollisionStatus(const CollisionStatus& collisionStatus)
    /// Method to set the global Goal
    /// ==================================================================================
    void SrlEBandPlannerROS::checkFrontLaserCollisionStatus(const CollisionStatus::ConstPtr& msg) {

        collision_error_front_    = msg->collisionError;
        //collision_error_front_    = msg->collisionError && (dir_planning_>0);
        collision_warning_front_  = msg->collisionWarning;
        // ROS_DEBUG("Srl Global Planner checking collision status, Error: %d, Warning: %d", collision_error_, collision_warning_);
        return;

    }

    /// ==================================================================================
    /// checkRearLaserCollisionStatus(const CollisionStatus& collisionStatus)
    /// Method to set the global Goal
    /// ==================================================================================
    void SrlEBandPlannerROS::checkRearLaserCollisionStatus(const CollisionStatus::ConstPtr& msg){

        collision_error_rear_   = msg->collisionError;
        //collision_error_rear_   = msg->collisionError && (dir_planning_<0);
        collision_warning_rear_  = msg->collisionWarning;
        // ROS_DEBUG("Srl Global Planner checking collision status, Error: %d, Warning: %d", collision_error_, collision_warning_);
        return;
    }

    /// ============================================================================
    // readVelocityCB, call back to read the current robot velocity
    /// ============================================================================
    void SrlEBandPlannerROS::readVelocityCB(const geometry_msgs::TwistStamped::ConstPtr& msg){

      double v = msg->twist.linear.x;
      double w = msg->twist.angular.z;
      // ROS_DEBUG_NAMED("Simple Head Behaviour", "Reading vel (%f, %f)", v, w);
      if(fabs(v)<EPS && fabs(w)<EPS){
        robot_still_position_ = true;
        // ROS_DEBUG_NAMED("Simple Head Behaviour","Robot still vel (%f, %f)", v, w);
      }else{
        robot_still_position_ = false;
      }
      return;
    }

    /// =======================================================================================
    /// callbackDynamicReconfigure
    /// =======================================================================================
    void SrlEBandPlannerROS::callbackDynamicReconfigure(srl_eband_local_planner::srlEBandLocalPlannerConfig &config, uint32_t level ){

      ROS_DEBUG("Reconfiguring Eband Planner");
      srlEBandLocalPlannerConfig config_int = config;
      number_tentative_setting_band_ = config.number_tentative_setting_band_dyn;
      check_costmap_layers_ = config.check_costmap_layers_dyn;
      eband_->callbackDynamicReconfigure(config,level);
      eband_trj_ctrl_->callbackDynamicReconfigure(config,level);
      return;

    }

    // set global plan to wrapper and pass it to eband
    bool SrlEBandPlannerROS::setPlan(const std::vector<geometry_msgs::PoseStamped>& orig_global_plan)
    {
      if(check_costmap_layers_)
        setCostmapsLayers();
        
      // check if plugin initialized
      if(!initialized_)
      {
        ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
        return false;
      }

      //reset the global plan
      global_plan_.clear();
      global_plan_ = orig_global_plan;

      // transform global plan to the map frame we are working in
      // this also cuts the plan off (reduces it to local window)
      std::vector<int> start_end_counts (2, (int) global_plan_.size()); // counts from the end() of the plan
      if(!srl_eband_local_planner::transformGlobalPlan(*tf_, global_plan_, *costmap_ros_, costmap_ros_->getGlobalFrameID(), transformed_plan_, start_end_counts))
      {
        // if plan could not be tranformed abort control and local planning
        ROS_WARN("Could not transform the global plan to the frame of the controller");
        return false;
      }

      // also check if there really is a plan
      if(transformed_plan_.empty())
      {
        // if global plan passed in is empty... we won't do anything
        ROS_WARN("Transformed plan is empty. Aborting local planner!");
        return false;
      }

      /// Update Map
      // costmap_ros_->updateMap();

      // set plan - as this is fresh from the global planner robot pose should be identical to start frame
      if(!eband_->setPlan(transformed_plan_))
      {
        // We've had some difficulty where the global planner keeps returning a valid path that runs through an obstacle
        // in the local costmap. See issue #5. Here we clear the local costmap and try one more time.
        // costmap_ros_->resetLayers(); /// TODO Testing it!!!
        int k = 0;
        bool plan_set = false;
        while (!plan_set && k<number_tentative_setting_band_) {

          ROS_WARN("Setting plan to Elastic Band method failed! Retrying to set plan #%d", k);
          costmap_ros_->resetLayers();
          plan_set = eband_->setPlan(transformed_plan_);
          ROS_WARN("Setting plan done, result %d",plan_set);
          if(check_costmap_layers_)
            setCostmapsLayers();
          k++;
        }

        if(!plan_set){
          ROS_ERROR("Setting plan to Elastic Band method failed!");
          return plan_set;
        }
        // if (!eband_->setPlan(transformed_plan_)) {
        //   ROS_ERROR("Setting plan to Elastic Band method failed!");
        //   return false;
        // }
        //
      }
      ROS_DEBUG("Global plan set to elastic band for optimization");

      // plan transformed and set to elastic band successfully - set counters to global variable
      plan_start_end_counter_ = start_end_counts;

      // let eband refine the plan before starting continuous operation (to smooth sampling based plans)

      if(!eband_->optimizeBand() && optimize_band_){

        ROS_DEBUG("optimizeBand failed, retrying .. ");
        // costmap_ros_->updateMap();
        // boost::this_thread::sleep(boost::posix_time::milliseconds(1000));
        int k = 0;
        bool band_optimized = false;
        while (!band_optimized && k<number_tentative_setting_band_) {

          ROS_WARN("optimizeBand failed! Retrying to optimizeBand #%d", k);
          costmap_ros_->resetLayers();
          band_optimized = eband_->optimizeBand();
          ROS_WARN("Setting optimizeBand done, result %d", band_optimized);
          k++;
        }

        if(!band_optimized){
          ROS_ERROR("optimizeBand method failed!");
          return band_optimized;
        }
        // if (!eband_->optimizeBand()) {
        //   ROS_ERROR("Setting plan to Elastic Band method failed! could not optimize band");
        //   return false;
        // }
      }


      // display result
      std::vector<srl_eband_local_planner::Bubble> current_band;
      if(eband_->getBand(current_band))
        eband_visual_->publishBand("bubbles", current_band);

      // set goal as not reached
      goal_reached_ = false;

      return true;
    }


    bool SrlEBandPlannerROS::computeVelocityCommands(geometry_msgs::Twist& cmd_vel)
    {
      // check if plugin initialized
      if(!initialized_)
      {
        ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
        return false;
      }

      if( (collision_error_front_ || collision_error_rear_) && robot_still_position_){

        ROS_ERROR("The local planner can't go on for a collision error, unstuck Behaviour");
        return false;

      }


      if(check_costmap_layers_)
        setCostmapsLayers();


//        ROS_INFO("LocalPlanner could not generate a path, collision error on, %d, %d, %d, %d", collision_error_rear_, collision_error_front_, robot_still_position_,dir_planning_);

      // instantiate local variables
      //std::vector<geometry_msgs::PoseStamped> local_plan;
      tf::Stamped<tf::Pose> global_pose;
      geometry_msgs::PoseStamped global_pose_msg;
      std::vector<geometry_msgs::PoseStamped> tmp_plan;

      // get curent robot position
      ROS_DEBUG("Reading current robot Position from costmap and appending it to elastic band.");
      if(!costmap_ros_->getRobotPose(global_pose))
      {
        ROS_WARN("Could not retrieve up to date robot pose from costmap for local planning.");
        return false;
      }

      // convert robot pose to frame in plan and set position in band at which to append
      tf::poseStampedTFToMsg(global_pose, global_pose_msg);
      tmp_plan.assign(1, global_pose_msg);
      srl_eband_local_planner::AddAtPosition add_frames_at = add_front;

      // set it to elastic band and let eband connect it
      if(!eband_->addFrames(tmp_plan, add_frames_at))
      {
        ROS_WARN("Could not connect robot pose to existing elastic band.");
        return false;
      }

      // get additional path-frames which are now in moving window
      ROS_DEBUG("Checking for new path frames in moving window");
      std::vector<int> plan_start_end_counter = plan_start_end_counter_;
      std::vector<geometry_msgs::PoseStamped> append_transformed_plan;
      // transform global plan to the map frame we are working in - careful this also cuts the plan off (reduces it to local window)
      if(!srl_eband_local_planner::transformGlobalPlan(*tf_, global_plan_, *costmap_ros_, costmap_ros_->getGlobalFrameID(), transformed_plan_, plan_start_end_counter))
      {
        // if plan could not be tranformed abort control and local planning
        ROS_WARN("Could not transform the global plan to the frame of the controller");
        return false;
      }

      // also check if there really is a plan
      if(transformed_plan_.empty())
      {
        // if global plan passed in is empty... we won't do anything
        ROS_WARN("Transformed plan is empty. Aborting local planner!");
        return false;
      }

      ROS_DEBUG("Retrieved start-end-counts are: (%d, %d)", plan_start_end_counter.at(0), plan_start_end_counter.at(1));
      ROS_DEBUG("Current start-end-counts are: (%d, %d)", plan_start_end_counter_.at(0), plan_start_end_counter_.at(1));

      // identify new frames - if there are any
      append_transformed_plan.clear();
      // did last transformed plan end futher away from end of complete plan than this transformed plan?
      if(plan_start_end_counter_.at(1) > plan_start_end_counter.at(1)) // counting from the back (as start might be pruned)
      {
        // new frames in moving window
        if(plan_start_end_counter_.at(1) > plan_start_end_counter.at(0)) // counting from the back (as start might be pruned)
        {
          // append everything
          append_transformed_plan = transformed_plan_;
        }
        else
        {
          // append only the new portion of the plan
          int discarded_frames = plan_start_end_counter.at(0) - plan_start_end_counter_.at(1);
          ROS_ASSERT(transformed_plan_.begin() + discarded_frames + 1 >= transformed_plan_.begin());
          ROS_ASSERT(transformed_plan_.begin() + discarded_frames + 1 < transformed_plan_.end());
          append_transformed_plan.assign(transformed_plan_.begin() + discarded_frames + 1, transformed_plan_.end());
        }

        // set it to elastic band and let eband connect it
        ROS_DEBUG("Adding %d new frames to current band", (int) append_transformed_plan.size());
        add_frames_at = add_back;
        if(eband_->addFrames(append_transformed_plan, add_back))
        {
          // appended frames succesfully to global plan - set new start-end counts
          ROS_DEBUG("Sucessfully added frames to band");
          plan_start_end_counter_ = plan_start_end_counter;
        }
        else {
          ROS_WARN("Failed to add frames to existing band");
          return false;
        }
      }
      else
        ROS_DEBUG("Nothing to add");

      // update Elastic Band (react on obstacle from costmap, ...)
      ROS_DEBUG("Calling optimization method for elastic band");
      std::vector<srl_eband_local_planner::Bubble> current_band;

      if(!eband_->optimizeBand() && optimize_band_)
      {
        ROS_WARN("Optimization failed - Band invalid - No controls availlable");
        /// TODO possible solution to avoid it but it may be risky
        // costmap_ros_->resetLayers();
        /// if a first attempt didn't work try again after a small pause
        boost::this_thread::sleep(boost::posix_time::milliseconds(200));
        if(!eband_->optimizeBand() && optimize_band_)
        {
          if(eband_->getBand(current_band))
            eband_visual_->publishBand("bubbles", current_band);

          return false;
        }
        // display current band
        // if(eband_->getBand(current_band))
        //   eband_visual_->publishBand("bubbles", current_band);
        // return false;
      }

      // get current Elastic Band and
      if(eband_->getBand(current_band))
        eband_visual_->publishBand("bubbles", current_band);

      // set it to the controller
      if(!eband_trj_ctrl_->setBand(current_band))
      {
        ROS_DEBUG("Failed to to set current band to Trajectory Controller");
        return false;
      }

      // set Odometry to controller
      ROS_DEBUG("set Odometry to controller");
      if(!eband_trj_ctrl_->setOdometry(base_odom_))
      {
        ROS_DEBUG("Failed to to set current odometry to Trajectory Controller");
        return false;
      }

      // get resulting commands from the controller
      ROS_DEBUG("get resulting commands from the controller");
      geometry_msgs::Twist cmd_twist;
      if(!eband_trj_ctrl_->getTwist(cmd_twist, goal_reached_))
      {
        ROS_DEBUG("Failed to calculate Twist from band in Trajectory Controller");
        return false;
      }


      // set retrieved commands to reference variable
      ROS_DEBUG("Retrieving velocity command: (%f, %f, %f)", cmd_twist.linear.x, cmd_twist.linear.y, cmd_twist.angular.z);
      cmd_vel = cmd_twist;


      // publish plan
      ROS_DEBUG("publish plan");

      std::vector<geometry_msgs::PoseStamped> refined_plan;
      if(eband_->getPlan(refined_plan))
        // TODO publish local and current gloabl plan
        base_local_planner::publishPlan(refined_plan, g_plan_pub_);
      //base_local_planner::publishPlan(local_plan, l_plan_pub_, 0.0, 0.0, 1.0, 0.0);

      // display current band
      if(eband_->getBand(current_band))
        eband_visual_->publishBand("bubbles", current_band);

      return true;
    }


    bool SrlEBandPlannerROS::isGoalReached()
    {
      // check if plugin initialized
      if(!initialized_)
      {
        ROS_ERROR("This planner has not been initialized, please call initialize() before using this planner");
        return false;
      }

      return goal_reached_;


    }


    void SrlEBandPlannerROS::odomCallback(const nav_msgs::Odometry::ConstPtr& msg)
    {
      // lock Callback while reading data from topic
      boost::mutex::scoped_lock lock(odom_mutex_);

      // get odometry and write it to member variable (we assume that the odometry is published in the frame of the base)
      base_odom_.twist.twist.linear.x = msg->twist.twist.linear.x;
      base_odom_.twist.twist.linear.y = msg->twist.twist.linear.y;
      base_odom_.twist.twist.angular.z = msg->twist.twist.angular.z;
    }


  }
