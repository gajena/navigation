/*********************************************************************
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2008, Willow Garage, Inc.
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
 *   * Neither the name of the Willow Garage nor the names of its
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
 * Author: Eitan Marder-Eppstein
 *********************************************************************/
#include <costmap_2d/costmap_2d_ros.h>

#include <nav_msgs/GetMap.h>
#include <limits>


namespace costmap_2d {

  double sign(double x){
    return x < 0.0 ? -1.0 : 1.0;
  }

  Costmap2DROS::Costmap2DROS(std::string name, tf::TransformListener& tf) : tf_(tf), costmap_(NULL), 
                             map_update_thread_(NULL), costmap_publisher_(NULL), stop_updates_(false), 
                             initialized_(true), stopped_(false), map_update_thread_shutdown_(false) {
    ros::NodeHandle nh(name);
    ros::NodeHandle private_nh("~/" + name);

    std::string map_type;
    private_nh.param("map_type", map_type, std::string("voxel"));

    private_nh.param("publish_voxel_map", publish_voxel_, false);

    if(publish_voxel_ && map_type == "voxel")
      voxel_pub_ = nh.advertise<costmap_2d::VoxelGrid>("voxel_grid", 1);
    else
      publish_voxel_ = false;

    std::string topics_string;
    //get the topics that we'll subscribe to from the parameter server
    private_nh.param("observation_sources", topics_string, std::string(""));
    ROS_INFO("Subscribed to Topics: %s", topics_string.c_str());

    private_nh.param("global_frame", global_frame_, std::string("/map"));
    private_nh.param("robot_base_frame", robot_base_frame_, std::string("base_link"));

    ros::Time last_error = ros::Time::now();
    std::string tf_error;
    //we need to make sure that the transform between the robot base frame and the global frame is available
    while(!tf_.waitForTransform(global_frame_, robot_base_frame_, ros::Time(), ros::Duration(0.1), ros::Duration(0.01), &tf_error)){
      ros::spinOnce();
      if(last_error + ros::Duration(5.0) < ros::Time::now()){
        ROS_WARN("Waiting on transform from %s to %s to become available before running costmap, tf error: %s", 
            robot_base_frame_.c_str(), global_frame_.c_str(), tf_error.c_str());
        last_error = ros::Time::now();
      }
    }

    private_nh.param("transform_tolerance", transform_tolerance_, 0.3);

    //now we need to split the topics based on whitespace which we can use a stringstream for
    std::stringstream ss(topics_string);

    double raytrace_range = 3.0;
    double obstacle_range = 2.5;

    std::string source;
    while(ss >> source){
      ros::NodeHandle source_node(private_nh, source);
      //get the parameters for the specific topic
      double observation_keep_time, expected_update_rate, min_obstacle_height, max_obstacle_height;
      std::string topic, sensor_frame, data_type;
      source_node.param("topic", topic, source);
      source_node.param("sensor_frame", sensor_frame, std::string(""));
      source_node.param("observation_persistence", observation_keep_time, 0.0);
      source_node.param("expected_update_rate", expected_update_rate, 0.0);
      source_node.param("data_type", data_type, std::string("PointCloud"));
      source_node.param("min_obstacle_height", min_obstacle_height, 0.05);
      source_node.param("max_obstacle_height", max_obstacle_height, 2.0);

      ROS_ASSERT_MSG(data_type == "PointCloud" || data_type == "LaserScan", "Only topics that use point clouds or laser scans are currently supported");


      bool clearing, marking;
      source_node.param("clearing", clearing, false);
      source_node.param("marking", marking, true);

      std::string raytrace_range_param_name, obstacle_range_param_name;
      double source_raytrace_range, source_obstacle_range;

      //get the obstacle range for the sensor
      if(!source_node.searchParam("obstacle_range", obstacle_range_param_name))
        source_obstacle_range = 2.5;
      else
        source_node.param(obstacle_range_param_name, source_obstacle_range, 2.5);

      //get the raytrace range for the sensor
      if(!source_node.searchParam("raytrace_range", raytrace_range_param_name))
        source_raytrace_range = 3.0;
      else
        source_node.param(raytrace_range_param_name, source_raytrace_range, 3.0);


      //keep track of the maximum raytrace range for the costmap to be able to inflate efficiently
      raytrace_range = std::max(raytrace_range, source_raytrace_range);
      obstacle_range = std::max(obstacle_range, source_obstacle_range);

      ROS_DEBUG("Creating an observation buffer for source %s, topic %s, frame %s", source.c_str(), topic.c_str(), sensor_frame.c_str());

      //create an observation buffer
      observation_buffers_.push_back(boost::shared_ptr<ObservationBuffer>(new ObservationBuffer(topic, observation_keep_time, 
              expected_update_rate, min_obstacle_height, max_obstacle_height, source_obstacle_range, source_raytrace_range, tf_, global_frame_, sensor_frame)));

      //check if we'll add this buffer to our marking observation buffers
      if(marking)
        marking_buffers_.push_back(observation_buffers_.back());

      //check if we'll also add this buffer to our clearing observation buffers
      if(clearing)
        clearing_buffers_.push_back(observation_buffers_.back());

      ROS_DEBUG("Created an observation buffer for source %s, topic %s, global frame: %s, expected update rate: %.2f, observation persistence: %.2f", 
          source.c_str(), topic.c_str(), global_frame_.c_str(), expected_update_rate, observation_keep_time);

      //create a callback for the topic
      if(data_type == "LaserScan"){
        observation_notifiers_.push_back(boost::shared_ptr<tf::MessageNotifierBase>(new tf::MessageNotifier<sensor_msgs::LaserScan>(tf_, 
              boost::bind(&Costmap2DROS::laserScanCallback, this, _1, observation_buffers_.back()), topic, global_frame_, 50)));
        observation_notifiers_.back()->setTolerance(ros::Duration(0.05));
      }
      else{
        observation_notifiers_.push_back(boost::shared_ptr<tf::MessageNotifierBase>(new tf::MessageNotifier<sensor_msgs::PointCloud>(tf_,
              boost::bind(&Costmap2DROS::pointCloudCallback, this, _1, observation_buffers_.back()), topic, global_frame_, 50)));
      }

      if(sensor_frame != ""){
        std::vector<std::string> target_frames;
        target_frames.push_back(global_frame_);
        target_frames.push_back(sensor_frame);
        observation_notifiers_.back()->setTargetFrame(target_frames);
      }

    }

    bool static_map;
    unsigned int map_width, map_height;
    double map_resolution;
    double map_origin_x, map_origin_y;

    private_nh.param("static_map", static_map, true);
    std::vector<unsigned char> input_data;

    //check if we want a rolling window version of the costmap
    private_nh.param("rolling_window", rolling_window_, false);

    double map_width_meters, map_height_meters;
    private_nh.param("width", map_width_meters, 10.0);
    private_nh.param("height", map_height_meters, 10.0);
    private_nh.param("resolution", map_resolution, 0.05);
    private_nh.param("origin_x", map_origin_x, 0.0);
    private_nh.param("origin_y", map_origin_y, 0.0);
    map_width = (unsigned int)(map_width_meters / map_resolution);
    map_height = (unsigned int)(map_height_meters / map_resolution);

    if(static_map){
      nav_msgs::GetMap::Request map_req;
      nav_msgs::GetMap::Response map_resp;
      ROS_INFO("Requesting the map...\n");
      while(!ros::service::call("/static_map", map_req, map_resp))
      {
        ROS_INFO("Request failed; trying again...\n");
        usleep(1000000);
      }
      ROS_INFO("Received a %d X %d map at %f m/pix\n",
          map_resp.map.info.width, map_resp.map.info.height, map_resp.map.info.resolution);

      //check if the user has set any parameters that will be overwritten
      bool user_map_params = false;
      user_map_params |= private_nh.hasParam("width");
      user_map_params |= private_nh.hasParam("height");
      user_map_params |= private_nh.hasParam("resolution");
      user_map_params |= private_nh.hasParam("origin_x");
      user_map_params |= private_nh.hasParam("origin_y");

      if(user_map_params)
        ROS_WARN("You have set map parameters, but also requested to use the static map. Your parameters will be overwritten by those given by the map server");

      // We are treating cells with no information as lethal obstacles based on the input data. This is not ideal but
      // our planner and controller do not reason about the no obstacle case
      unsigned int numCells = map_resp.map.info.width * map_resp.map.info.height;
      for(unsigned int i = 0; i < numCells; i++){
        input_data.push_back((unsigned char) map_resp.map.data[i]);
      }

      map_width = (unsigned int)map_resp.map.info.width;
      map_height = (unsigned int)map_resp.map.info.height;
      map_resolution = map_resp.map.info.resolution;
      map_origin_x = map_resp.map.info.origin.position.x;
      map_origin_y = map_resp.map.info.origin.position.y;

    }

    double inscribed_radius, circumscribed_radius, inflation_radius;
    inscribed_radius = 0.46;

    //BEGIN TODO 0.4: REMOVE DEPRECATION STUFF HERE
    if(private_nh.hasParam("circumscribed_radius")){
      ROS_WARN("The circumscribed_radius parameter no longer has an affect as the radius is automatically computed from the robot footprint. Please remove the parameter from you launch file.");
    }

    if(private_nh.hasParam("inscribed_radius")){
      ROS_WARN("The \"inscribed_radius\" parameter has been deprecated in favor of the \"robot_radius\" parameter. Please switch your launch/configuration files");
      private_nh.param("inscirbed_radius", inscribed_radius, 0.46);
    }

    if(private_nh.hasParam("robot_radius")){
      private_nh.param("robot_radius", inscribed_radius, 0.46);
    }
    //END TODO

    circumscribed_radius = inscribed_radius;
    private_nh.param("inflation_radius", inflation_radius, 0.55);

    //load the robot footprint from the parameter server if its available in the global namespace
    footprint_spec_ = loadRobotFootprint(private_nh, inscribed_radius, circumscribed_radius);

    if(footprint_spec_.size() > 2){
      //now we need to compute the inscribed/circumscribed radius of the robot from the footprint specification
      double min_dist = std::numeric_limits<double>::max();
      double max_dist = 0.0;

      for(unsigned int i = 0; i < footprint_spec_.size() - 1; ++i){
        //check the distance from the robot center point to the first vertex
        double vertex_dist = distance(0.0, 0.0, footprint_spec_[i].x, footprint_spec_[i].y);
        double edge_dist = distanceToLine(0.0, 0.0, footprint_spec_[i].x, footprint_spec_[i].y, footprint_spec_[i+1].x, footprint_spec_[i+1].y);
        min_dist = std::min(min_dist, std::min(vertex_dist, edge_dist));
        max_dist = std::max(max_dist, std::max(vertex_dist, edge_dist));
      }

      //we also need to do the last vertex and the first vertex
      double vertex_dist = distance(0.0, 0.0, footprint_spec_.back().x, footprint_spec_.back().y);
      double edge_dist = distanceToLine(0.0, 0.0, footprint_spec_.back().x, footprint_spec_.back().y, footprint_spec_.front().x, footprint_spec_.front().y);
      min_dist = std::min(min_dist, std::min(vertex_dist, edge_dist));
      max_dist = std::max(max_dist, std::max(vertex_dist, edge_dist));

      inscribed_radius = min_dist;
      circumscribed_radius = max_dist;
    }

    double max_obstacle_height;
    private_nh.param("max_obstacle_height", max_obstacle_height, 2.0);

    double cost_scale;
    private_nh.param("cost_scaling_factor", cost_scale, 10.0);

    int temp_lethal_threshold;
    private_nh.param("lethal_cost_threshold", temp_lethal_threshold, int(100));

    unsigned char lethal_threshold = std::max(std::min(temp_lethal_threshold, 255), 0);

    struct timeval start, end;
    double start_t, end_t, t_diff;
    gettimeofday(&start, NULL);
    if(map_type == "costmap"){
      costmap_ = new Costmap2D(map_width, map_height,
          map_resolution, map_origin_x, map_origin_y, inscribed_radius, circumscribed_radius, inflation_radius,
          obstacle_range, max_obstacle_height, raytrace_range, cost_scale, input_data, lethal_threshold);
    }
    else if(map_type == "voxel"){

      int z_voxels;
      private_nh.param("z_voxels", z_voxels, 10);

      double z_resolution, map_origin_z;
      private_nh.param("z_resolution", z_resolution, 0.2);
      private_nh.param("origin_z", map_origin_z, 0.0);

      int unknown_threshold, mark_threshold;
      private_nh.param("unknown_threshold", unknown_threshold, z_voxels);
      private_nh.param("mark_threshold", mark_threshold, 0);

      ROS_ASSERT(z_voxels >= 0 && unknown_threshold >= 0 && mark_threshold >= 0);

      costmap_ = new VoxelCostmap2D(map_width, map_height, z_voxels, map_resolution, z_resolution, map_origin_x, map_origin_y, map_origin_z, inscribed_radius,
          circumscribed_radius, inflation_radius, obstacle_range, raytrace_range, cost_scale, input_data, lethal_threshold, unknown_threshold, mark_threshold);
    }
    else{
      ROS_ASSERT_MSG(false, "Unsuported map type");
    }

    gettimeofday(&end, NULL);
    start_t = start.tv_sec + double(start.tv_usec) / 1e6;
    end_t = end.tv_sec + double(end.tv_usec) / 1e6;
    t_diff = end_t - start_t;
    ROS_DEBUG("New map construction time: %.9f", t_diff);

    double map_publish_frequency;
    private_nh.param("publish_frequency", map_publish_frequency, 0.0);

    //create a publisher for the costmap if desired
    costmap_publisher_ = new Costmap2DPublisher(nh, map_publish_frequency, global_frame_);
    if(costmap_publisher_->active()){
      std::vector<geometry_msgs::Point> oriented_footprint;
      getOrientedFootprint(oriented_footprint);
      tf::Stamped<tf::Pose> global_pose;
      getRobotPose(global_pose);
      costmap_publisher_->updateCostmapData(*costmap_, oriented_footprint, global_pose);
    }

    //create a thread to handle updating the map
    double map_update_frequency;
    private_nh.param("update_frequency", map_update_frequency, 5.0);
    map_update_thread_ = new boost::thread(boost::bind(&Costmap2DROS::mapUpdateLoop, this, map_update_frequency));

  }

  double Costmap2DROS::distanceToLine(double pX, double pY, double x0, double y0, double x1, double y1){
    double A = pX - x0;
    double B = pY - y0;
    double C = x1 - x0;
    double D = y1 - y0;

    double dot = A * C + B * D;
    double len_sq = C * C + D * D;
    double param = dot / len_sq;

    double xx,yy;

    if(param < 0)
    {
      xx = x0;
      yy = y0;
    }
    else if(param > 1)
    {
      xx = x1;
      yy = y1;
    }
    else
    {
      xx = x0 + param * C;
      yy = y0 + param * D;
    }

    return distance(pX,pY,xx,yy);
  }

  std::vector<geometry_msgs::Point> Costmap2DROS::loadRobotFootprint(ros::NodeHandle node, double inscribed_radius, double circumscribed_radius){
    std::vector<geometry_msgs::Point> footprint;
    geometry_msgs::Point pt;
    double padding;

    std::string padding_param, footprint_param;
    if(!node.searchParam("footprint_padding", padding_param))
      padding = 0.01;
    else
      node.param(padding_param, padding, 0.01);

    //grab the footprint from the parameter server if possible
    XmlRpc::XmlRpcValue footprint_list;
    if(node.searchParam("footprint", footprint_param)){
      node.getParam(footprint_param, footprint_list);
      //make sure we have a list of lists
      ROS_ASSERT_MSG(footprint_list.getType() == XmlRpc::XmlRpcValue::TypeArray && footprint_list.size() > 2, 
          "The footprint must be specified as list of lists on the parameter server with at least 3 points eg: [[x1, y1], [x2, y2], ..., [xn, yn]]");
      for(int i = 0; i < footprint_list.size(); ++i){
        //make sure we have a list of lists of size 2
        XmlRpc::XmlRpcValue point = footprint_list[i];
        ROS_ASSERT_MSG(point.getType() == XmlRpc::XmlRpcValue::TypeArray && point.size() == 2, 
            "The footprint must be specified as list of lists on the parameter server eg: [[x1, y1], [x2, y2], ..., [xn, yn]], but this spec is not of that form");

        //make sure that the value we're looking at is either a double or an int
        ROS_ASSERT(point[0].getType() == XmlRpc::XmlRpcValue::TypeInt || point[0].getType() == XmlRpc::XmlRpcValue::TypeDouble);
        pt.x = point[0].getType() == XmlRpc::XmlRpcValue::TypeInt ? (int)(point[0]) : (double)(point[0]);
        pt.x += sign(pt.x) * padding;

        //make sure that the value we're looking at is either a double or an int
        ROS_ASSERT(point[1].getType() == XmlRpc::XmlRpcValue::TypeInt || point[1].getType() == XmlRpc::XmlRpcValue::TypeDouble);
        pt.y = point[1].getType() == XmlRpc::XmlRpcValue::TypeInt ? (int)(point[1]) : (double)(point[1]);
        pt.y += sign(pt.y) * padding;

        footprint.push_back(pt);

      }
    }
    return footprint;
  }

  Costmap2DROS::~Costmap2DROS(){
    map_update_thread_shutdown_ = true;
    if(map_update_thread_ != NULL){
      map_update_thread_->join();
      delete map_update_thread_;
    }

    if(costmap_publisher_ != NULL){
      delete costmap_publisher_;
    }

    if(costmap_ != NULL)
      delete costmap_;
  }

  void Costmap2DROS::start(){
    //check if we're stopped or just paused
    if(stopped_){
      //if we're stopped we need to re-subscribe to topics
      for(unsigned int i = 0; i < observation_notifiers_.size(); ++i){
        if(observation_notifiers_[i] != NULL)
          observation_notifiers_[i]->subscribeToMessage();
      }
      stopped_ = false;
    }
    stop_updates_ = false;

    //block until the costmap is re-initialized.. meaning one update cycle has run
    ros::Rate r(100.0);
    while(!initialized_)
      r.sleep();
  }

  void Costmap2DROS::stop(){
    stop_updates_ = true;
    //unsubscribe from topics
    for(unsigned int i = 0; i < observation_notifiers_.size(); ++i){
      if(observation_notifiers_[i] != NULL)
        observation_notifiers_[i]->unsubscribeFromMessage();
    }
    initialized_ = false;
    stopped_ = true;
  }

  void Costmap2DROS::addObservationBuffer(const boost::shared_ptr<ObservationBuffer>& buffer){
    if(buffer)
      observation_buffers_.push_back(buffer);
  }

  void Costmap2DROS::laserScanCallback(const tf::MessageNotifier<sensor_msgs::LaserScan>::MessagePtr& message, const boost::shared_ptr<ObservationBuffer>& buffer){
    //project the laser into a point cloud
    sensor_msgs::PointCloud base_cloud;
    base_cloud.header = message->header;

    //project the scan into a point cloud
    try
    {
      projector_.transformLaserScanToPointCloud(message->header.frame_id, base_cloud, *message, tf_);
    }
    catch (tf::TransformException &ex)
    {
      ROS_WARN ("High fidelity enabled, but TF returned a transform exception to frame %s: %s", global_frame_.c_str (), ex.what ());
      projector_.projectLaser(*message, base_cloud);
    }

    //buffer the point cloud
    buffer->lock();
    buffer->bufferCloud(base_cloud);
    buffer->unlock();
  }

  void Costmap2DROS::pointCloudCallback(const tf::MessageNotifier<sensor_msgs::PointCloud>::MessagePtr& message, const boost::shared_ptr<ObservationBuffer>& buffer){
    //buffer the point cloud
    buffer->lock();
    buffer->bufferCloud(*message);
    buffer->unlock();
  }

  void Costmap2DROS::mapUpdateLoop(double frequency){
    //the user might not want to run the loop every cycle
    if(frequency == 0.0)
      return;

    ros::NodeHandle nh;
    ros::Rate r(frequency);
    while(nh.ok() && !map_update_thread_shutdown_){
      struct timeval start, end;
      double start_t, end_t, t_diff;
      gettimeofday(&start, NULL);
      if(!stop_updates_){
        updateMap();
        initialized_ = true;
      }
      gettimeofday(&end, NULL);
      start_t = start.tv_sec + double(start.tv_usec) / 1e6;
      end_t = end.tv_sec + double(end.tv_usec) / 1e6;
      t_diff = end_t - start_t;
      ROS_DEBUG("Map update time: %.9f", t_diff);

      r.sleep();
      //make sure to sleep for the remainder of our cycle time
      if(r.cycleTime() > ros::Duration(1 / frequency))
        ROS_WARN("Map update loop missed its desired rate of %.4fHz... the loop actually took %.4f seconds", frequency, r.cycleTime().toSec());
    }
  }

  bool Costmap2DROS::getMarkingObservations(std::vector<Observation>& marking_observations){
    bool current = true;
    //get the marking observations
    for(unsigned int i = 0; i < marking_buffers_.size(); ++i){
      marking_buffers_[i]->lock();
      marking_buffers_[i]->getObservations(marking_observations);
      current = marking_buffers_[i]->isCurrent() && current;
      marking_buffers_[i]->unlock();
    }
    return current;
  }

  bool Costmap2DROS::getClearingObservations(std::vector<Observation>& clearing_observations){
    bool current = true;
    //get the clearing observations
    for(unsigned int i = 0; i < clearing_buffers_.size(); ++i){
      clearing_buffers_[i]->lock();
      clearing_buffers_[i]->getObservations(clearing_observations);
      current = clearing_buffers_[i]->isCurrent() && current;
      clearing_buffers_[i]->unlock();
    }
    return current;
  }


  void Costmap2DROS::updateMap(){
    tf::Stamped<tf::Pose> global_pose;
    if(!getRobotPose(global_pose))
      return;

    double wx = global_pose.getOrigin().x();
    double wy = global_pose.getOrigin().y();

    bool current = true;
    std::vector<Observation> observations, clearing_observations;

    //get the marking observations
    current = current && getMarkingObservations(observations);

    //get the clearing observations
    current = current && getClearingObservations(clearing_observations);

    //update the global current status
    current_ = current;

    boost::recursive_mutex::scoped_lock lock(lock_);
    //if we're using a rolling buffer costmap... we need to update the origin using the robot's position
    if(rolling_window_){
      double origin_x = wx - costmap_->getSizeInMetersX() / 2;
      double origin_y = wy - costmap_->getSizeInMetersY() / 2;
      costmap_->updateOrigin(origin_x, origin_y);
    }
    costmap_->updateWorld(wx, wy, observations, clearing_observations);

    //make sure to clear the robot footprint of obstacles at the end
    clearRobotFootprint();

    //if we have an active publisher... we'll update its costmap data
    if(costmap_publisher_->active()){
      std::vector<geometry_msgs::Point> oriented_footprint;
      getOrientedFootprint(oriented_footprint);
      tf::Stamped<tf::Pose> global_pose;
      getRobotPose(global_pose);
      costmap_publisher_->updateCostmapData(*costmap_, oriented_footprint, global_pose);
    }

    if(publish_voxel_){
      costmap_2d::VoxelGrid voxel_grid;
      ((VoxelCostmap2D*)costmap_)->getVoxelGridMessage(voxel_grid);
      voxel_grid.header.frame_id = global_frame_;
      voxel_grid.header.stamp = ros::Time::now();
      voxel_pub_.publish(voxel_grid);
    }

  }

  void Costmap2DROS::clearNonLethalWindow(double size_x, double size_y){
    tf::Stamped<tf::Pose> global_pose;
    if(!getRobotPose(global_pose))
      return;

    double wx = global_pose.getOrigin().x();
    double wy = global_pose.getOrigin().y();
    lock_.lock();
    ROS_DEBUG("Clearing map in window");
    costmap_->clearNonLethal(wx, wy, size_x, size_y, true);
    lock_.unlock();

    //make sure to force an update of the map to take in the latest sensor data
    updateMap();
  }

  void Costmap2DROS::resetMapOutsideWindow(double size_x, double size_y){
    tf::Stamped<tf::Pose> global_pose;
    if(!getRobotPose(global_pose))
      return;

    double wx = global_pose.getOrigin().x();
    double wy = global_pose.getOrigin().y();
    lock_.lock();
    ROS_DEBUG("Resetting map outside window");
    costmap_->resetMapOutsideWindow(wx, wy, size_x, size_y);
    lock_.unlock();

    //make sure to force an update of the map to take in the latest sensor data
    updateMap();

  }

  void Costmap2DROS::getCostmapCopy(Costmap2D& costmap){
    boost::recursive_mutex::scoped_lock lock(lock_);
    costmap = *costmap_;
  }

  unsigned int Costmap2DROS::getSizeInCellsX() {
    boost::recursive_mutex::scoped_lock lock(lock_);
    return costmap_->getSizeInCellsX();
  }

  unsigned int Costmap2DROS::getSizeInCellsY() {
    boost::recursive_mutex::scoped_lock lock(lock_);
    return costmap_->getSizeInCellsY();
  }

  double Costmap2DROS::getResolution() {
    boost::recursive_mutex::scoped_lock lock(lock_);
    return costmap_->getResolution();
  }

  bool Costmap2DROS::getRobotPose(tf::Stamped<tf::Pose>& global_pose){
    global_pose.setIdentity();
    tf::Stamped<tf::Pose> robot_pose;
    robot_pose.setIdentity();
    robot_pose.frame_id_ = robot_base_frame_;
    robot_pose.stamp_ = ros::Time();
    ros::Time current_time = ros::Time::now(); // save time for checking tf delay later

    //get the global pose of the robot
    try{
      tf_.transformPose(global_frame_, robot_pose, global_pose);
    }
    catch(tf::LookupException& ex) {
      ROS_ERROR("No Transform available Error: %s\n", ex.what());
      return false;
    }
    catch(tf::ConnectivityException& ex) {
      ROS_ERROR("Connectivity Error: %s\n", ex.what());
      return false;
    }
    catch(tf::ExtrapolationException& ex) {
      ROS_ERROR("Extrapolation Error: %s\n", ex.what());
      return false;
    }
    // check global_pose timeout
    if (current_time.toSec() - global_pose.stamp_.toSec() > transform_tolerance_) {
      ROS_WARN("Costmap2DROS transform timeout. Current time: %.4f, global_pose stamp: %.4f, tolerance: %.4f",
          current_time.toSec() ,global_pose.stamp_.toSec() ,transform_tolerance_);
      return false;
    }

    return true;
  }

  void Costmap2DROS::clearRobotFootprint(){
    tf::Stamped<tf::Pose> global_pose;
    if(!getRobotPose(global_pose))
      return;

    clearRobotFootprint(global_pose);
  }

  std::vector<geometry_msgs::Point> Costmap2DROS::getRobotFootprint(){
    return footprint_spec_;
  }

  void Costmap2DROS::getOrientedFootprint(std::vector<geometry_msgs::Point>& oriented_footprint){
    tf::Stamped<tf::Pose> global_pose;
    if(!getRobotPose(global_pose))
      return;

    double useless_pitch, useless_roll, yaw;
    global_pose.getBasis().getEulerZYX(yaw, useless_pitch, useless_roll);

    getOrientedFootprint(global_pose.getOrigin().x(), global_pose.getOrigin().y(), yaw, oriented_footprint);
  }

  void Costmap2DROS::getOrientedFootprint(double x, double y, double theta, std::vector<geometry_msgs::Point>& oriented_footprint){
    //build the oriented footprint at the robot's current location
    double cos_th = cos(theta);
    double sin_th = sin(theta);
    for(unsigned int i = 0; i < footprint_spec_.size(); ++i){
      geometry_msgs::Point new_pt;
      new_pt.x = x + (footprint_spec_[i].x * cos_th - footprint_spec_[i].y * sin_th);
      new_pt.y = y + (footprint_spec_[i].x * sin_th + footprint_spec_[i].y * cos_th);
      oriented_footprint.push_back(new_pt);
    }
  }

  bool Costmap2DROS::setConvexPolygonCost(const std::vector<geometry_msgs::Point>& polygon, unsigned char cost_value){
    lock_.lock();
    bool success = costmap_->setConvexPolygonCost(polygon, costmap_2d::FREE_SPACE);
    lock_.unlock();

    //make sure to take our active sensor data into account
    updateMap();

    return success;
  }

  std::string Costmap2DROS::getGlobalFrameID(){
    return global_frame_;
  }

  std::string Costmap2DROS::getBaseFrameID(){
    return robot_base_frame_;
  }

  double Costmap2DROS::getInscribedRadius(){
    boost::recursive_mutex::scoped_lock lock(lock_);
    return costmap_->getInscribedRadius();
  }

  double Costmap2DROS::getCircumscribedRadius(){
    boost::recursive_mutex::scoped_lock lock(lock_);
    return costmap_->getCircumscribedRadius();
  }

  double Costmap2DROS::getInflationRadius(){
    boost::recursive_mutex::scoped_lock lock(lock_);
    return costmap_->getInflationRadius();
  }

  void Costmap2DROS::clearRobotFootprint(const tf::Stamped<tf::Pose>& global_pose){
    //if we don't have a footprint of a large enough size, we'll just clear the cell that the robot occupies
    if(footprint_spec_.size() < 3){
      //lock the map if necessary
      boost::recursive_mutex::scoped_lock lock(lock_);
      unsigned int mx, my;
      if(costmap_->worldToMap(global_pose.getOrigin().x(), global_pose.getOrigin().y(), mx, my)){
        costmap_->setCost(mx, my, costmap_2d::FREE_SPACE);
      }
      return;
    }

    double useless_pitch, useless_roll, yaw;
    global_pose.getBasis().getEulerZYX(yaw, useless_pitch, useless_roll);

    //get the oriented footprint of the robot
    double x = global_pose.getOrigin().x();
    double y = global_pose.getOrigin().y();
    double theta = yaw;

    //build the oriented footprint at the robot's current location
    std::vector<geometry_msgs::Point> oriented_footprint;
    getOrientedFootprint(x, y, theta, oriented_footprint);

    //lock the map if necessary
    boost::recursive_mutex::scoped_lock lock(lock_);

    //set the associated costs in the cost map to be free
    if(!costmap_->setConvexPolygonCost(oriented_footprint, costmap_2d::FREE_SPACE))
      return;

    double max_inflation_dist = costmap_->getInflationRadius() + costmap_->getCircumscribedRadius();

    //clear all non-lethal obstacles out to the maximum inflation distance of an obstacle in the robot footprint
    costmap_->clearNonLethal(global_pose.getOrigin().x(), global_pose.getOrigin().y(), max_inflation_dist, max_inflation_dist);

    //make sure to re-inflate obstacles in the affected region... plus those obstalces that could inflate to have costs in the footprint
    costmap_->reinflateWindow(global_pose.getOrigin().x(), global_pose.getOrigin().y(), 
        max_inflation_dist + costmap_->getInflationRadius(), max_inflation_dist + costmap_->getInflationRadius(), false);
  }

};