#include <manipulation_actions/ClutteredGrasper.h>

using std::ios;
using std::string;
using std::stringstream;
using std::vector;

ClutteredGrasper::ClutteredGrasper() :
    pnh_("~"),
    cloud_(new pcl::PointCloud<pcl::PointXYZRGB>),
    sample_grasps_client_("/rail_agile/sample_grasps"),
    gripper_client("gripper_controller/gripper_action"),
    blind_bin_pick_server(pnh_, "blind_bin_pick", boost::bind(&ClutteredGrasper::executeBlind, this, _1), false),
    smart_bin_pick_server(pnh_, "smart_bin_pick", boost::bind(&ClutteredGrasper::executeSmart, this, _1), false)
{
  string segmentation_topic, cloud_topic;
  pnh_.param<string>("segmentation_topic", segmentation_topic, "rail_segmentation/segmented_objects");
  pnh_.param<string>("cloud_topic", cloud_topic, "head_camera/depth_registered/points");
  pnh_.param<double>("box_dim_x", box_dims.x, 0.17);
  pnh_.param<double>("box_dim_y", box_dims.y, 0.17);
  pnh_.param<double>("box_error_threshold", box_error_threshold, 0.05);

  cloud_received_ = false;

  gripper_names_.push_back("gripper_link");
  gripper_names_.push_back("l_gripper_finger_link");
  gripper_names_.push_back("r_gripper_finger_link");

  cloud_subscriber_ = n_.subscribe(cloud_topic, 1, &ClutteredGrasper::cloudCallback, this);
  objects_subscriber_ = n_.subscribe(segmentation_topic, 1, &ClutteredGrasper::objectsCallback, this);
  planning_scene_publisher_ = n_.advertise<moveit_msgs::PlanningScene>("/planning_scene", 1);

  arm_group = new moveit::planning_interface::MoveGroupInterface("arm");
  arm_group->startStateMonitor();

  planning_scene_client_ = n_.serviceClient<moveit_msgs::GetPlanningScene>("/get_planning_scene");
  attach_arbitrary_object_client =
      n_.serviceClient<manipulation_actions::AttachArbitraryObject>("collision_scene_manager/attach_arbitrary_object");
  cartesian_path_client = n_.serviceClient<moveit_msgs::GetCartesianPath>("/compute_cartesian_path");

  grasps_publisher_ = pnh_.advertise<geometry_msgs::PoseArray>("grasps_debug", 1);
  box_pose_publisher = pnh_.advertise<geometry_msgs::PoseStamped>("box_pose_debug", 1);
  current_grasp_publisher = pnh_.advertise<geometry_msgs::PoseStamped>("current_grasp_debug", 1);
  sample_cloud_publisher = pnh_.advertise<pcl::PointCloud<pcl::PointXYZRGB> >("sample_cloud_debug", 1);
}

void ClutteredGrasper::executeBlind(const manipulation_actions::BinPickGoalConstPtr &goal)
{
  boost::mutex::scoped_lock lock(object_list_mutex_);

  manipulation_actions::BinPickResult result;

  // find the screw box and execute grasp calculation
  size_t box_index = object_list_.objects.size();
  double min_error = std::numeric_limits<double>::max();
  for (size_t i = 0; i < object_list_.objects.size(); i ++)
  {
    double obj_x = object_list_.objects[i].bounding_volume.dimensions.y;
    double obj_y = object_list_.objects[i].bounding_volume.dimensions.z;
    if (obj_y > obj_x)
    {
      double temp = obj_x;
      obj_x = obj_y;
      obj_y = temp;
    }
    double error = fabs(obj_x - box_dims.x) + fabs(obj_y - box_dims.y);
    std::cout << "Object " << i << ": " << object_list_.objects[i].bounding_volume.dimensions.x << ", " <<
              object_list_.objects[i].bounding_volume.dimensions.y << ", " << object_list_.objects[i].bounding_volume.dimensions.z << std::endl;
    if (error <= box_error_threshold && error < min_error)
    {
      min_error = error;
      box_index = i;
    }
  }

  if (box_index < object_list_.objects.size())
  {
    ROS_INFO("Screw box found at index %lu", box_index);
    rail_manipulation_msgs::SegmentedObject screw_box = object_list_.objects[box_index];

    geometry_msgs::PoseStamped box_pose;
    box_pose.header.frame_id = screw_box.point_cloud.header.frame_id;
    box_pose.pose.position = screw_box.center;
    box_pose.pose.orientation = screw_box.bounding_volume.pose.pose.orientation;
    box_pose_publisher.publish(box_pose);

    // TODO: check what this pose is, rotate it if necessary so x-axis points down
    geometry_msgs::PoseStamped approach_pose;
    approach_pose.header.frame_id = box_pose.header.frame_id;
    approach_pose.pose = box_pose.pose;
    approach_pose.pose.position.z += 0.15;

    // rotation code example (if needed)
//    geometry_msgs::PoseStamped pose_candidate;
//    tf2::Transform place_object_tf;
//    tf2::fromMsg(object_pose.pose, place_object_tf);
//
//    // optional 180 degree rotation about z-axis to cover all x-axis pose alignments
//    tf2::Quaternion initial_adjustment;
//    initial_adjustment.setRPY(0, 0, j*M_PI);
//    // rotate pose around x-axis to generate candidates (longest axis, which most constrains place)
//    tf2::Quaternion adjustment;
//    adjustment.setRPY(i * M_PI_4, 0, 0);
//    place_object_tf.setRotation(place_object_tf.getRotation() * initial_adjustment * adjustment);
//
//    // determine wrist frame pose that will give the desired grasp
//    tf2::Transform place_candidate_tf;
//    place_candidate_tf = place_object_tf * object_to_wrist_tf;
//
//    pose_candidate.header.frame_id = object_pose.header.frame_id;
//    pose_candidate.pose.position.x = place_candidate_tf.getOrigin().x();
//    pose_candidate.pose.position.y = place_candidate_tf.getOrigin().y();
//    pose_candidate.pose.position.z = place_candidate_tf.getOrigin().z();
//    pose_candidate.pose.orientation = tf2::toMsg(place_candidate_tf.getRotation());

    ROS_INFO("Moving to pick approach pose...");
    arm_group->setPlannerId("arm[RRTConnectkConfigDefault]");
    arm_group->setPlanningTime(5.0);
    arm_group->setStartStateToCurrentState();
    arm_group->setPoseTarget(approach_pose, "gripper_link");

    moveit::planning_interface::MoveItErrorCode move_result = arm_group->move();
    if (move_result != moveit_msgs::MoveItErrorCodes::SUCCESS)
    {
      blind_bin_pick_server.setAborted(result);
      return;
    }

    // open gripper
    control_msgs::GripperCommandGoal gripper_goal;
    gripper_goal.command.position = 1.0;
    gripper_goal.command.max_effort = 200;
    gripper_client.sendGoal(gripper_goal);
    gripper_client.waitForResult(ros::Duration(5.0));

    //disable collision between gripper links and object
    moveit_msgs::GetPlanningScene planning_scene_srv;
    planning_scene_srv.request.components.components = moveit_msgs::PlanningSceneComponents::ALLOWED_COLLISION_MATRIX;

    if (!planning_scene_client_.call(planning_scene_srv))
    {
      ROS_INFO("Could not get the current planning scene!");
    }
    else
    {
      collision_detection::AllowedCollisionMatrix acm(planning_scene_srv.response.scene.allowed_collision_matrix);
      // disable collisions between gripper links and octomap
      acm.setEntry("<octomap>", gripper_names_, true);
      moveit_msgs::PlanningScene planning_scene_update;
      acm.getMessage(planning_scene_update.allowed_collision_matrix);
      planning_scene_update.is_diff = true;
      planning_scene_publisher_.publish(planning_scene_update);

      ros::Duration(0.5).sleep(); //delay for publish to go through
    }



//    // TODO: check that this path is safe before moving (i.e. no big jump)
//    ROS_INFO("Moving to pick pose...");
//    arm_group->setPlannerId("arm[RRTConnectkConfigDefault]");
//    arm_group->setPlanningTime(5.0);
//    arm_group->setStartStateToCurrentState();
//    arm_group->setPoseTarget(approach_pose, "gripper_link");

    move_result = arm_group->move();
    if (move_result != moveit_msgs::MoveItErrorCodes::SUCCESS)
    {
      blind_bin_pick_server.setAborted(result);
      return;
    }

    // close gripper
    gripper_goal.command.position = 0.0;
    gripper_goal.command.max_effort = 200;
    gripper_client.sendGoal(gripper_goal);
    gripper_client.waitForResult(ros::Duration(5.0));

    // attach an arbitrary bolt object
    manipulation_actions::AttachArbitraryObject attach_srv;
    attach_srv.request.challenge_object.object = manipulation_actions::ChallengeObject::BOLT;
    if (!attach_arbitrary_object_client.call(attach_srv))
    {
      ROS_INFO("Could not call moveit collision scene manager service!");
    }

    // re-enable gripper collision with octomap
    planning_scene_srv.request.components.components = moveit_msgs::PlanningSceneComponents::ALLOWED_COLLISION_MATRIX;
    if (!planning_scene_client_.call(planning_scene_srv))
    {
      ROS_INFO("Could not get the current planning scene!");
    }
    else
    {
      collision_detection::AllowedCollisionMatrix acm(planning_scene_srv.response.scene.allowed_collision_matrix);
      acm.setEntry("<octomap>", gripper_names_, false);

      moveit_msgs::PlanningScene planning_scene_update;
      acm.getMessage(planning_scene_update.allowed_collision_matrix);
      planning_scene_update.is_diff = true;
      planning_scene_publisher_.publish(planning_scene_update);
    }
  }
  else
  {
    ROS_INFO("No screw box found among segmented objects.");
    blind_bin_pick_server.setAborted(result);
  }

  blind_bin_pick_server.setSucceeded(result);
}

bool ClutteredGrasper::executeCartesianMove(geometry_msgs::PoseStamped goal)
{
  moveit_msgs::GetCartesianPath grasp_path;

  //TODO: currently assumes goal is the pose of gripper_link in the base_link frame
  geometry_msgs::Pose goal_pose;
  goal_pose = goal.pose;
  goal_pose.position.x -= .166;  // gripper link to wrist_roll_link offset
  grasp_path.request.waypoints.push_back(goal_pose);

  grasp_path.request.max_step = 0.01;
  grasp_path.request.jump_threshold = 1.5;  // From nimbus
  grasp_path.request.avoid_collisions = false;
  grasp_path.request.group_name = "arm";
  moveit::core::robotStateToRobotStateMsg(*(arm_group->getCurrentState()), grasp_path.request.start_state);

  int max_planning_attempts = 3;
  for (int num_attempts=0; num_attempts < max_planning_attempts; num_attempts++)
  {
    ROS_INFO("Attempting to plan path to grasp. Attempt: %d/%d",
             num_attempts + 1, max_planning_attempts);
    if (grasp_path.response.fraction >= 0.5)
    {
      ROS_INFO("Succeeded in computing %f of the path to grasp",
               grasp_path.response.fraction);
      break;
    }
    else if (!cartesian_path_client.call(grasp_path)
             || grasp_path.response.fraction < 0
             || num_attempts >= max_planning_attempts - 1)
    {
      ROS_INFO("Could not calculate a Cartesian path for grasp!");
      return false;
    }
  }

  //execute the grasp plan
  moveit::planning_interface::MoveGroupInterface::Plan grasp_plan;
  grasp_plan.trajectory_ = grasp_path.response.solution;
  moveit::core::robotStateToRobotStateMsg(*(arm_group->getCurrentState()), grasp_plan.start_state_);
  int error_code = arm_group->execute(grasp_plan).val;
  if (error_code == moveit_msgs::MoveItErrorCodes::PREEMPTED)
  {
    ROS_INFO("Preempted while moving to executing grasp.");
    return false;
  }
  else if (error_code != moveit_msgs::MoveItErrorCodes::SUCCESS)
  {
    ROS_INFO("Cartesian path failed to execute.");
    return false;
  }

  ROS_INFO("Cartesian path successfully executed.");
  return true;
}

void ClutteredGrasper::executeSmart(const manipulation_actions::BinPickGoalConstPtr &goal)
{
  boost::mutex::scoped_lock lock(object_list_mutex_);

  manipulation_actions::BinPickResult result;

  // find the screw box and execute grasp calculation
  size_t box_index = object_list_.objects.size();
  double min_error = std::numeric_limits<double>::max();
  for (size_t i = 0; i < object_list_.objects.size(); i ++)
  {
    double obj_x = object_list_.objects[i].bounding_volume.dimensions.y;
    double obj_y = object_list_.objects[i].bounding_volume.dimensions.z;
    if (obj_y > obj_x)
    {
      double temp = obj_x;
      obj_x = obj_y;
      obj_y = temp;
    }
    double error = fabs(obj_x - box_dims.x) + fabs(obj_y - box_dims.y);
    std::cout << "Object " << i << ": " << object_list_.objects[i].bounding_volume.dimensions.x << ", " <<
              object_list_.objects[i].bounding_volume.dimensions.y << ", " << object_list_.objects[i].bounding_volume.dimensions.z << std::endl;
    if (error <= box_error_threshold && error < min_error)
    {
      min_error = error;
      box_index = i;
    }
  }

  if (box_index < object_list_.objects.size())
  {
    ROS_INFO("Screw box found at index %lu", box_index);
    rail_manipulation_msgs::SegmentedObject screw_box = object_list_.objects[box_index];

    geometry_msgs::PoseStamped box_pose;
    box_pose.header.frame_id = screw_box.point_cloud.header.frame_id;
    box_pose.pose.position = screw_box.center;
    box_pose.pose.orientation = screw_box.bounding_volume.pose.pose.orientation;
    box_pose_publisher.publish(box_pose);

    // remove edges of box from point cloud
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr object_cloud(new pcl::PointCloud<pcl::PointXYZRGB>);
    pcl::PCLPointCloud2::Ptr temp_cloud(new pcl::PCLPointCloud2);
    pcl_conversions::toPCL(screw_box.point_cloud, *temp_cloud);
    pcl::fromPCLPointCloud2(*temp_cloud, *object_cloud);
    tf::Transform box_frame_tf;
    box_frame_tf.setOrigin(tf::Vector3(screw_box.center.x, screw_box.center.y, screw_box.center.z));
    box_frame_tf.setRotation(tf::Quaternion(screw_box.bounding_volume.pose.pose.orientation.x, screw_box.bounding_volume.pose.pose.orientation.y, screw_box.bounding_volume.pose.pose.orientation.z, screw_box.bounding_volume.pose.pose.orientation.w));
    pcl::CropBox<pcl::PointXYZRGB> crop_box;
    Eigen::Vector4f min_point, max_point;
    double box_edge_removal = 0.05;
    max_point[0] = static_cast<float>(screw_box.bounding_volume.dimensions.x/2.0);
    max_point[1] = static_cast<float>(screw_box.bounding_volume.dimensions.y/2.0 - box_edge_removal);
    max_point[2] = static_cast<float>(screw_box.bounding_volume.dimensions.z/2.0 - box_edge_removal);
    min_point[0] = static_cast<float>(-max_point[0] - 0.1);
    min_point[1] = static_cast<float>(-max_point[1]);
    min_point[2] = static_cast<float>(-max_point[2]);
    crop_box.setMin(min_point);
    crop_box.setMax(max_point);
    crop_box.setTranslation(Eigen::Vector3f(screw_box.center.x, screw_box.center.y, screw_box.center.z));
    double roll, pitch, yaw;
    tf::Matrix3x3(box_frame_tf.getRotation()).getRPY(roll, pitch, yaw);
    crop_box.setRotation(Eigen::Vector3f(static_cast<float>(roll), static_cast<float>(pitch), static_cast<float>(yaw)));

    pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed_cloud(new pcl::PointCloud<pcl::PointXYZRGB>);
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr cropped_cloud(new pcl::PointCloud<pcl::PointXYZRGB>);
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr sample_cloud(new pcl::PointCloud<pcl::PointXYZRGB>);
    pcl_ros::transformPointCloud(screw_box.point_cloud.header.frame_id, ros::Time(0), *cloud_, cloud_->header.frame_id,
                                 *transformed_cloud, tf_listener_);
    transformed_cloud->header.frame_id = screw_box.point_cloud.header.frame_id;

    crop_box.setInputCloud(transformed_cloud);
    crop_box.filter(*cropped_cloud);

    pcl_ros::transformPointCloud(cloud_->header.frame_id, ros::Time(0), *cropped_cloud, screw_box.point_cloud.header.frame_id,
                                 *sample_cloud, tf_listener_);
    sample_cloud->header.frame_id = cloud_->header.frame_id;

    // sample grasps
    sample_cloud_publisher.publish(sample_cloud);
    geometry_msgs::PoseArray grasps;
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_out(new pcl::PointCloud<pcl::PointXYZRGB>);
//    sampleGraspCandidates(screw_box.point_cloud, screw_box.point_cloud.header.frame_id, cloud_->header.frame_id, grasps, cloud_out);
    sampleGraspCandidates2(sample_cloud, grasps);

    grasps_publisher_.publish(grasps);
    ROS_INFO("%lu grasps calculated and published in frame %s.", grasps.poses.size(), grasps.header.frame_id.c_str());
    if (!grasps.poses.empty())
    {
      geometry_msgs::PoseStamped current_grasp;
      current_grasp.header.frame_id = grasps.header.frame_id;
      current_grasp.pose = grasps.poses[0];
      current_grasp_publisher.publish(current_grasp);
    }
  }
  else
  {
    ROS_INFO("Screw box not found among segmented objects.");
  }

  // TODO: execution (this is unfinished because blind picks seem more effective anyway...)
  smart_bin_pick_server.setSucceeded(result);
}

void ClutteredGrasper::cloudCallback(const pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr &msg)
{
  boost::mutex::scoped_lock lock(cloud_mutex_);

  *cloud_ = *msg;

  cloud_received_ = true;
}

void ClutteredGrasper::sampleGraspCandidates(sensor_msgs::PointCloud2 object, string object_source_frame,
    string environment_source_frame, geometry_msgs::PoseArray &grasps_out,
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_out)
{
  //get a pcl version of the object point cloud
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr object_cloud(new pcl::PointCloud<pcl::PointXYZRGB>);
  pcl::PCLPointCloud2::Ptr temp_cloud(new pcl::PCLPointCloud2);
  pcl_conversions::toPCL(object, *temp_cloud);
  pcl::fromPCLPointCloud2(*temp_cloud, *object_cloud);

  //transform object cloud to camera frame to get new crop box dimensions
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed_cloud(new pcl::PointCloud<pcl::PointXYZRGB>);
  if (object_cloud->header.frame_id != environment_source_frame)
  {
    pcl_ros::transformPointCloud(environment_source_frame, ros::Time(0), *object_cloud, object_source_frame,
                                 *transformed_cloud, tf_listener_);
    transformed_cloud->header.frame_id = environment_source_frame;
  }
  else
  {
    *transformed_cloud = *object_cloud;
  }

  //calculate workspace bounds in new coordinate frame
  pcl::PointXYZRGB min_workspace_point, max_workspace_point;
  pcl::getMinMax3D(*transformed_cloud, min_workspace_point, max_workspace_point);

  //crop cloud based on specified object
  //set padding to a negative number to not calculate grasps on the box edge
  double cloud_padding = -0.04;
  pcl::CropBox<pcl::PointXYZRGB> crop_box;
  Eigen::Vector4f min_point, max_point;
  min_point[0] = static_cast<float>(min_workspace_point.x - cloud_padding);
  min_point[1] = static_cast<float>(min_workspace_point.y - cloud_padding);
  min_point[2] = static_cast<float>(min_workspace_point.z - cloud_padding);
  max_point[0] = static_cast<float>(max_workspace_point.x + cloud_padding);
  max_point[1] = static_cast<float>(max_workspace_point.y + cloud_padding);
  max_point[2] = static_cast<float>(max_workspace_point.z + cloud_padding);
  crop_box.setMin(min_point);
  crop_box.setMax(max_point);
  crop_box.setInputCloud(cloud_);
  crop_box.filter(*cloud_out);
  sample_cloud_publisher.publish(cloud_out);

  rail_grasp_calculation_msgs::SampleGraspsGoal sample_goal;
  pcl::toPCLPointCloud2(*cloud_out, *temp_cloud);
  pcl_conversions::fromPCL(*temp_cloud, sample_goal.cloud);

  sample_goal.workspace.mode = rail_grasp_calculation_msgs::Workspace::WORKSPACE_VOLUME;
  sample_goal.workspace.x_min = min_point[0];
  sample_goal.workspace.y_min = min_point[1];
  sample_goal.workspace.z_min = min_point[2];
  sample_goal.workspace.x_max = max_point[0];
  sample_goal.workspace.y_max = max_point[1];
  sample_goal.workspace.z_max = max_point[2];

  sample_grasps_client_.sendGoal(sample_goal);
  sample_grasps_client_.waitForResult(ros::Duration(10.0));
  grasps_out = sample_grasps_client_.getResult()->graspList;
}

void ClutteredGrasper::sampleGraspCandidates2(pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud, geometry_msgs::PoseArray &grasps_out)
{
  //calculate workspace bounds in new coordinate frame
  pcl::PointXYZRGB min_workspace_point, max_workspace_point;
  pcl::getMinMax3D(*cloud, min_workspace_point, max_workspace_point);

  //crop cloud based on specified object
  //set padding to a negative number to not calculate grasps on the box edge
  double cloud_padding = 0;
//  pcl::CropBox<pcl::PointXYZRGB> crop_box;
  Eigen::Vector4f min_point, max_point;
  min_point[0] = static_cast<float>(min_workspace_point.x - cloud_padding);
  min_point[1] = static_cast<float>(min_workspace_point.y - cloud_padding);
  min_point[2] = static_cast<float>(min_workspace_point.z - cloud_padding);
  max_point[0] = static_cast<float>(max_workspace_point.x + cloud_padding);
  max_point[1] = static_cast<float>(max_workspace_point.y + cloud_padding);
  max_point[2] = static_cast<float>(max_workspace_point.z + cloud_padding);
//  crop_box.setMin(min_point);
//  crop_box.setMax(max_point);
//  crop_box.setInputCloud(cloud_);
//  crop_box.filter(*cloud_out);
//  sample_cloud_publisher.publish(cloud_out);

  pcl::PCLPointCloud2::Ptr temp_cloud(new pcl::PCLPointCloud2);
  rail_grasp_calculation_msgs::SampleGraspsGoal sample_goal;
  pcl::toPCLPointCloud2(*cloud, *temp_cloud);
  pcl_conversions::fromPCL(*temp_cloud, sample_goal.cloud);

  sample_goal.workspace.mode = rail_grasp_calculation_msgs::Workspace::WORKSPACE_VOLUME;
  sample_goal.workspace.x_min = min_point[0];
  sample_goal.workspace.y_min = min_point[1];
  sample_goal.workspace.z_min = min_point[2];
  sample_goal.workspace.x_max = max_point[0];
  sample_goal.workspace.y_max = max_point[1];
  sample_goal.workspace.z_max = max_point[2];

  sample_grasps_client_.sendGoal(sample_goal);
  sample_grasps_client_.waitForResult(ros::Duration(10.0));
  grasps_out = sample_grasps_client_.getResult()->graspList;
}

void ClutteredGrasper::objectsCallback(const rail_manipulation_msgs::SegmentedObjectList &list)
{
  boost::mutex::scoped_lock lock(object_list_mutex_);

  object_list_ = list;

  // for testing, find the screw box and execute grasp calculation
  size_t box_index = object_list_.objects.size();
  double min_error = std::numeric_limits<double>::max();
  for (size_t i = 0; i < object_list_.objects.size(); i ++)
  {
    //TODO: check on rail_segmentation's bounding box calculation ordering of (x,y,z) dims; also this should be
    // documented somewhere!
    double obj_x = object_list_.objects[i].bounding_volume.dimensions.y;
    double obj_y = object_list_.objects[i].bounding_volume.dimensions.z;
    if (obj_y > obj_x)
    {
      double temp = obj_x;
      obj_x = obj_y;
      obj_y = temp;
    }
    double error = fabs(obj_x - box_dims.x) + fabs(obj_y - box_dims.y);
    std::cout << "Object " << i << ": " << object_list_.objects[i].bounding_volume.dimensions.x << ", " <<
    object_list_.objects[i].bounding_volume.dimensions.y << ", " << object_list_.objects[i].bounding_volume.dimensions.z << std::endl;
    if (error <= box_error_threshold && error < min_error)
    {
      min_error = error;
      box_index = i;
    }
  }

  if (box_index < object_list_.objects.size())
  {
    ROS_INFO("Screw box found at index %lu", box_index);
    rail_manipulation_msgs::SegmentedObject screw_box = object_list_.objects[box_index];

    geometry_msgs::PoseStamped box_pose;
    box_pose.header.frame_id = screw_box.point_cloud.header.frame_id;
    box_pose.pose.position = screw_box.center;
    box_pose.pose.orientation = screw_box.bounding_volume.pose.pose.orientation;
    box_pose_publisher.publish(box_pose);

    // remove edges of box from point cloud
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr object_cloud(new pcl::PointCloud<pcl::PointXYZRGB>);
    pcl::PCLPointCloud2::Ptr temp_cloud(new pcl::PCLPointCloud2);
    pcl_conversions::toPCL(screw_box.point_cloud, *temp_cloud);
    pcl::fromPCLPointCloud2(*temp_cloud, *object_cloud);
    tf::Transform box_frame_tf;
    box_frame_tf.setOrigin(tf::Vector3(screw_box.center.x, screw_box.center.y, screw_box.center.z));
    box_frame_tf.setRotation(tf::Quaternion(screw_box.bounding_volume.pose.pose.orientation.x, screw_box.bounding_volume.pose.pose.orientation.y, screw_box.bounding_volume.pose.pose.orientation.z, screw_box.bounding_volume.pose.pose.orientation.w));
    pcl::CropBox<pcl::PointXYZRGB> crop_box;
    Eigen::Vector4f min_point, max_point;
    double box_edge_removal = 0.05;
    max_point[0] = static_cast<float>(screw_box.bounding_volume.dimensions.x/2.0);
    max_point[1] = static_cast<float>(screw_box.bounding_volume.dimensions.y/2.0 - box_edge_removal);
    max_point[2] = static_cast<float>(screw_box.bounding_volume.dimensions.z/2.0 - box_edge_removal);
    min_point[0] = static_cast<float>(-max_point[0] - 0.1);
    min_point[1] = static_cast<float>(-max_point[1]);
    min_point[2] = static_cast<float>(-max_point[2]);
    crop_box.setMin(min_point);
    crop_box.setMax(max_point);
    crop_box.setTranslation(Eigen::Vector3f(screw_box.center.x, screw_box.center.y, screw_box.center.z));
    double roll, pitch, yaw;
    tf::Matrix3x3(box_frame_tf.getRotation()).getRPY(roll, pitch, yaw);
    crop_box.setRotation(Eigen::Vector3f(static_cast<float>(roll), static_cast<float>(pitch), static_cast<float>(yaw)));

    pcl::PointCloud<pcl::PointXYZRGB>::Ptr transformed_cloud(new pcl::PointCloud<pcl::PointXYZRGB>);
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr cropped_cloud(new pcl::PointCloud<pcl::PointXYZRGB>);
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr sample_cloud(new pcl::PointCloud<pcl::PointXYZRGB>);
    pcl_ros::transformPointCloud(screw_box.point_cloud.header.frame_id, ros::Time(0), *cloud_, cloud_->header.frame_id,
                                 *transformed_cloud, tf_listener_);
    transformed_cloud->header.frame_id = screw_box.point_cloud.header.frame_id;

    crop_box.setInputCloud(transformed_cloud);
    crop_box.filter(*cropped_cloud);

    pcl_ros::transformPointCloud(cloud_->header.frame_id, ros::Time(0), *cropped_cloud, screw_box.point_cloud.header.frame_id,
                                 *sample_cloud, tf_listener_);
    sample_cloud->header.frame_id = cloud_->header.frame_id;

    // sample grasps
    sample_cloud_publisher.publish(sample_cloud);
    geometry_msgs::PoseArray grasps;
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_out(new pcl::PointCloud<pcl::PointXYZRGB>);
//    sampleGraspCandidates(screw_box.point_cloud, screw_box.point_cloud.header.frame_id, cloud_->header.frame_id, grasps, cloud_out);
    sampleGraspCandidates2(sample_cloud, grasps);

    grasps_publisher_.publish(grasps);
    ROS_INFO("%lu grasps calculated and published in frame %s.", grasps.poses.size(), grasps.header.frame_id.c_str());
    if (!grasps.poses.empty())
    {
      geometry_msgs::PoseStamped current_grasp;
      current_grasp.header.frame_id = grasps.header.frame_id;
      current_grasp.pose = grasps.poses[0];
      current_grasp_publisher.publish(current_grasp);
    }
  }
  else
  {
    ROS_INFO("Screw box not found among segmented objects.");
  }
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "cluttered_grasper");

  ClutteredGrasper cg;

  ros::spin();

  return EXIT_SUCCESS;
}