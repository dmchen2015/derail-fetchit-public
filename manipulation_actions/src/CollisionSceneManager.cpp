#include <manipulation_actions/CollisionSceneManager.h>

using std::ios;
using std::string;
using std::stringstream;
using std::vector;

CollisionSceneManager::CollisionSceneManager() :
    pnh("~"),
    tf_listener(tf_buffer)
{
//  pnh.param<bool>("debug", debug, true);

  arm_group = new moveit::planning_interface::MoveGroupInterface("arm");
  arm_group->startStateMonitor();
  planning_scene_interface = new moveit::planning_interface::PlanningSceneInterface();

  attach_closest_server = pnh.advertiseService("attach_closest_object", &CollisionSceneManager::attachClosestObject, this);
  detach_all_server = pnh.advertiseService("detach_objects", &CollisionSceneManager::detachAllObjects, this);
  attach_arbitrary_server= pnh.advertiseService("attach_arbitrary_object", &CollisionSceneManager::attachArbitraryObject, this);

  objects_subscriber = n.subscribe("rail_segmentation/segmented_objects", 1, &CollisionSceneManager::objectsCallback, this);
}

void CollisionSceneManager::objectsCallback(const rail_manipulation_msgs::SegmentedObjectList &msg)
{
  //remove previously detected collision objects
  unattached_objects.clear();  //clear list of unattached scene object names
  vector<string> previous_objects = planning_scene_interface->getKnownObjectNames();
  if (!attached_objects.empty())
  {
    //don't remove the attached object
    for (int i = 0; i < previous_objects.size(); i ++)
    {
      for (size_t j = 0; j < attached_objects.size(); j ++)
      {
        if (previous_objects[i] == attached_objects[j])
        {
          previous_objects.erase(previous_objects.begin() + i);
          i --;
          break;
        }
      }
    }
  }
  planning_scene_interface->removeCollisionObjects(previous_objects);

  {
    boost::mutex::scoped_lock lock(objects_mutex); //lock for the stored objects array

    //store objects
    object_list = msg;

    if (!msg.objects.empty())
    {
      //add all objects to the planning scene
      vector<moveit_msgs::CollisionObject> collision_objects;
      collision_objects.resize(msg.objects.size());
      for (unsigned int i = 0; i < collision_objects.size(); i++)
      {
        //create collision object
        collision_objects[i].header.frame_id = msg.objects[i].point_cloud.header.frame_id;
        stringstream ss;
        if (msg.objects[i].recognized)
          ss << msg.objects[i].name << i;
        else
          ss << "object" << i;
        //check for name collisions
        for (unsigned int j = 0; j < attached_objects.size(); j ++)
        {
          if (ss.str() == attached_objects[j])
            ss << "_";
        }
        collision_objects[i].id = ss.str();
        unattached_objects.push_back(ss.str());

        //set object shape
        shape_msgs::SolidPrimitive bounding_volume;
        bounding_volume.type = shape_msgs::SolidPrimitive::BOX;
        bounding_volume.dimensions.resize(3);
        bounding_volume.dimensions[shape_msgs::SolidPrimitive::BOX_X] = msg.objects[i].bounding_volume.dimensions.x;
        bounding_volume.dimensions[shape_msgs::SolidPrimitive::BOX_Y] = msg.objects[i].bounding_volume.dimensions.y;
        bounding_volume.dimensions[shape_msgs::SolidPrimitive::BOX_Z] = msg.objects[i].bounding_volume.dimensions.z;
        collision_objects[i].primitives.push_back(bounding_volume);
        collision_objects[i].primitive_poses.push_back(msg.objects[i].bounding_volume.pose.pose);
        collision_objects[i].operation = moveit_msgs::CollisionObject::ADD;
      }

      planning_scene_interface->addCollisionObjects(collision_objects);
    }
  }
}

bool CollisionSceneManager::attachClosestObject(std_srvs::Empty::Request &req, std_srvs::Empty::Response &res)
{
  boost::mutex::scoped_lock lock(objects_mutex);  //lock for the stored objects array

  //find the closest point to the gripper pose
  size_t closest = 0;
  if (object_list.objects.size() == 0)
  {
    ROS_INFO("No scene objects to attach.");
    return true;
  }
  else if (object_list.objects.size() > 1)
  {
    // find the closest point
    double min = std::numeric_limits<double>::infinity();
    // check each segmented object
    for (size_t i = 0; i < object_list.objects.size(); i++)
    {
      geometry_msgs::TransformStamped eef_transform = tf_buffer.lookupTransform(
          object_list.objects[i].point_cloud.header.frame_id, "gripper_link", ros::Time(0));
      geometry_msgs::Vector3 &v = eef_transform.transform.translation;
      //convert PointCloud2 to PointCloud to access the data easily
      sensor_msgs::PointCloud cloud;
      sensor_msgs::convertPointCloud2ToPointCloud(object_list.objects[i].point_cloud, cloud);
      // check each point in the cloud
      for (size_t j = 0; j < cloud.points.size(); j++)
      {
        // euclidean distance to the point
        double dist_sqr = pow(cloud.points[j].x - v.x, 2) + pow(cloud.points[j].y - v.y, 2)
            + pow(cloud.points[j].z - v.z, 2);
        if (dist_sqr < min)
        {
          min = dist_sqr;
          closest = i;
        }
      }
    }

    if (min > SCENE_OBJECT_DST_SQR_THRESHOLD)
    {
      ROS_INFO("No scene objects are close enough to the end effector to be attached.");
      return true;
    }
  }

  ROS_INFO("Attaching scene object %lu to gripper.", closest);
  vector<string> touch_links;
  touch_links.emplace_back("r_gripper_finger_link");
  touch_links.emplace_back("l_gripper_finger_link");
  touch_links.emplace_back("gripper_link");
  touch_links.emplace_back("wrist_roll_link");
  touch_links.emplace_back("wrist_flex_link");
  arm_group->attachObject(unattached_objects[closest], arm_group->getEndEffectorLink(), touch_links);
  attached_objects.push_back(unattached_objects[closest]);
  unattached_objects.erase(unattached_objects.begin() + closest);

  return true;
}

bool CollisionSceneManager::detachAllObjects(std_srvs::Empty::Request &req, std_srvs::Empty::Response &res)
{
  for (int i = 0; i < attached_objects.size(); i ++)
  {
    arm_group->detachObject(attached_objects[i]);
  }
  planning_scene_interface->removeCollisionObjects(attached_objects);
  attached_objects.clear();

  return true;
}

bool CollisionSceneManager::attachArbitraryObject(manipulation_actions::AttachArbitraryObject::Request &req,
    manipulation_actions::AttachArbitraryObject::Response &res)
{
  // add an arbitrary object to planning scene for testing (typically this would be done at grasp time)
  vector<moveit_msgs::CollisionObject> collision_objects;
  collision_objects.resize(1);
  collision_objects[0].header.frame_id = "gripper_link";
  std::stringstream obj_name("arbitrary_");
  shape_msgs::SolidPrimitive shape;
  shape.type = shape_msgs::SolidPrimitive::SPHERE;
  shape.dimensions.resize(1);
  if (req.object == manipulation_actions::ChallengeObject::BOLT)
  {
    shape.dimensions[shape_msgs::SolidPrimitive::SPHERE_RADIUS] = 0.065;
    obj_name << "bolt";
  }
  else if (req.object == manipulation_actions::ChallengeObject::SMALL_GEAR)
  {
    shape.dimensions[shape_msgs::SolidPrimitive::SPHERE_RADIUS] = 0.045;
    obj_name << "small_gear";
  }
  else if (req.object == manipulation_actions::ChallengeObject::LARGE_GEAR)
  {
    shape.dimensions[shape_msgs::SolidPrimitive::SPHERE_RADIUS] = 0.115;
    obj_name << "large_gear";
  }
  else if (req.object == manipulation_actions::ChallengeObject::GEARBOX_TOP)
  {
    shape.dimensions[shape_msgs::SolidPrimitive::SPHERE_RADIUS] = 0.175;
    obj_name << "gearbox_top";
  }
  else if (req.object == manipulation_actions::ChallengeObject::GEARBOX_BOTTOM)
  {
    shape.dimensions[shape_msgs::SolidPrimitive::SPHERE_RADIUS] = 0.175;
    obj_name << "gearbox_bottom";
  }
  else
  {
    shape.dimensions[shape_msgs::SolidPrimitive::SPHERE_RADIUS] = 0.15;
    obj_name << "object";
  }
  collision_objects[0].id = obj_name.str();
  collision_objects[0].primitives.push_back(shape);
  geometry_msgs::Pose pose;
  pose.orientation.w = 1.0;
  collision_objects[0].primitive_poses.push_back(pose);
  planning_scene_interface->addCollisionObjects(collision_objects);

  ros::Duration(0.5).sleep();

  vector<string> touch_links;
  touch_links.emplace_back("r_gripper_finger_link");
  touch_links.emplace_back("l_gripper_finger_link");
  touch_links.emplace_back("gripper_link");
  touch_links.emplace_back("wrist_roll_link");
  touch_links.emplace_back("wrist_flex_link");
  arm_group->attachObject("arbitrary_gripper_object", "gripper_link", touch_links);

  return true;
}


int main(int argc, char **argv)
{
  ros::init(argc, argv, "collision_scene_manager");

  CollisionSceneManager csm;

  ros::spin();

  return EXIT_SUCCESS;
}