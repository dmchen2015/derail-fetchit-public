#include <ros/ros.h>
#include <visualization_msgs/Marker.h>
#include <pcl/conversions.h>
#include <pcl_conversions/pcl_conversions.h>
#include <sensor_msgs/PointCloud2.h>
#include <std_srvs/Empty.h>

#include <tf2_ros/static_transform_broadcaster.h>
#include <tf2_ros/transform_broadcaster.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/TransformStamped.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#include <tf2/LinearMath/Quaternion.h>

#include <gsl/gsl_fit.h>

#include <vector>
#include <utility>

#include "rail_manipulation_msgs/ProcessSegmentedObjects.h"
#include "rail_manipulation_msgs/SegmentObjects.h"
#include "fetchit_bin_detector/GetBinPose.h"
#include "ApproxMVBB/ComputeApproxMVBB.hpp"
#include "manipulation_actions/AttachToBase.h"
#include "fetchit_icp/TemplateMatch.h"


class BinDetector {
    public:
        BinDetector(ros::NodeHandle& nh, const std::string& seg_node, const std::string& seg_frame,
                         const std::string& kit_icp_node, bool viz);
        // gets the index of the minimum extent for the bounding box (i.e. shortest side)
        void minExtent(ApproxMVBB::OOBB& bb, ApproxMVBB::Vector3::Index& i);
        // sets z-axis as the minimum extent of the bounding box
        void setZAxisShortest(ApproxMVBB::OOBB& bb);
        // inverts z-axis of the bounding box
        void invertZAxis(ApproxMVBB::OOBB& bb);
        // get scale of the bounding box in meters
        ApproxMVBB::Vector3 get_box_scale(ApproxMVBB::OOBB& bb);
        // gets center of the bounding box in meters in the world frame
        ApproxMVBB::Vector3 get_box_center(ApproxMVBB::OOBB& bb);
        // gets pose of the bounding box in meters in the world frame
        ApproxMVBB::Vector3 get_box_pose(ApproxMVBB::OOBB& bb);
        // gets pose of the TOP of the bounding box in meters in the bin frame
        ApproxMVBB::Vector3 get_box_top_in_bin(ApproxMVBB::OOBB& bb);
        // gets pose of the TOP of the bounding box in meters in the world frame
        ApproxMVBB::Vector3 get_box_top_in_world(ApproxMVBB::OOBB& bb);
        // bin pose detection service handler
        bool handle_bin_pose_service(fetchit_bin_detector::GetBinPose::Request& req, fetchit_bin_detector::GetBinPose::Response& res);
        // gets the bin orientation
        bool get_bin_pose(ApproxMVBB::OOBB& bb, sensor_msgs::PointCloud2& cloud, geometry_msgs::Pose& bin_pose);
        // gets bin handle's slope in segmentation frame
        float get_handle_slope_from_cloud(ApproxMVBB::OOBB& bb, pcl::PointCloud<pcl::PointXYZRGB>& cloud);
        // checks if a slope is aligned to the x-axis
        bool slopeAlignedToXAxis(float handle_slope, ApproxMVBB::OOBB& bb, pcl::PointCloud<pcl::PointXYZRGB>& cloud);
        // checks if bin's second wall is aligned to x-axis
        int secondWallAlignedToXAxis(ApproxMVBB::Quaternion adjust_orientation, ApproxMVBB::OOBB& bb, pcl::PointCloud<pcl::PointXYZRGB>& cloud);
        // visualizes bin pose
        void visualize_bb(int id, geometry_msgs::Pose bin_pose);
        // publish the transform for the best (closest) bin
        void publish_bin_tf();
        // refines the bin pose estimate using ICP
        bool icp_refined_pose(sensor_msgs::PointCloud2 icp_cloud_msg, geometry_msgs::Pose& initial, geometry_msgs::Pose& final, double& matching_error);


    protected:
        ros::NodeHandle nh_, pnh_;
        ros::ServiceClient seg_client_;
        ros::ServiceClient merge_client_;
        ros::ServiceClient attach_base_client_;
        ros::ServiceClient detach_base_client_;
        ros::ServiceClient icp_client_;
        ros::ServiceServer pose_srv_;
        ros::Publisher vis_pub_;
        ros::Subscriber table_sub_;
        std::string seg_frame_;
        bool visualize_;

    private:
        geometry_msgs::TransformStamped best_bin_transform_;
        geometry_msgs::TransformStamped base_right_bin_transform_;
        geometry_msgs::TransformStamped base_left_bin_transform_;
        bool bin_detected_;
        tf2_ros::TransformBroadcaster tf_broadcaster_;
        //ros::Publisher pub2_; //TODO DEBUG

        double table_height_;
        bool table_received_;
        bool debug_;

        void table_callback(const rail_manipulation_msgs::SegmentedObject &msg);
};
