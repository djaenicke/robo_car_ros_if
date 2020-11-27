#include "ros/ros.h"
#include "robo_car_ros_if/state.h"
#include "robo_car_ros_if/footprint.h"
#include "robo_car_ros_if/cals.h"

#include "robot_localization/SetPose.h"

#include <tf/transform_broadcaster.h>
#include <tf/LinearMath/Matrix3x3.h>
#include <nav_msgs/Odometry.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <sensor_msgs/Imu.h>
#include <signal.h>
#include <vector>

#define XX 0
#define XY 1
#define XZ 2
#define YX 3
#define YY 4
#define YZ 5
#define ZX 6
#define ZY 7
#define ZZ 8

#define MPU_AX_VARIANCE 0.000191916
#define MPU_AY_VARIANCE 0.000207292

#define MPU_GZ_VARIANCE 2.95646E-05

#define FXOS_AX_VARIANCE 2.71782E-05
#define FXOS_AY_VARIANCE 4.62584E-05

static void InitEkfMsgs(void);
static void InitPoseCallBack(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& msg);
static void StateMsgUpdateCallBack(const robo_car_ros_if::state::ConstPtr& msg);
static void PublishOdometry(void);

// Pubs and Subs
static ros::Subscriber state_sub;
static ros::Subscriber init_pose_sub;
static ros::Publisher odom_pub;
static ros::Publisher footprint_pub;
static ros::Publisher imu_mpu_pub;
static ros::Publisher imu_fxos_pub;

// ServiceClients
static ros::ServiceClient reset_ekf_pose;

static nav_msgs::Odometry odom;
static sensor_msgs::Imu mpu;
static sensor_msgs::Imu fxos;

static ros::Time current_time, last_time;
static double x_dot = 0;
static double x = 0;
static double y = 0;
static double th = 0;
static double th_dot = 0;

int main(int argc, char **argv)
{
  ros::init(argc, argv, "pose_estimation");
  ros::NodeHandle nh;

  current_time = ros::Time::now();
  last_time = ros::Time::now();

  state_sub = nh.subscribe("robo_car_state", 100, StateMsgUpdateCallBack);
  init_pose_sub = nh.subscribe("initialpose", 100, InitPoseCallBack);

  reset_ekf_pose = nh.serviceClient<robot_localization::SetPose>("/set_pose");

  odom_pub = nh.advertise<nav_msgs::Odometry>("odom", 100);
  footprint_pub = nh.advertise<geometry_msgs::PolygonStamped>("robot_footprint", 100);
  imu_mpu_pub = nh.advertise<sensor_msgs::Imu>("imu_mpu", 100);
  imu_fxos_pub = nh.advertise<sensor_msgs::Imu>("imu_fxos", 100);

  // Configure the robot's footprint for rviz
  int num_points;
  nh.getParam("robot_footprint_points", num_points);
  robo_car_ros_if::Footprint robot_footprint = robo_car_ros_if::Footprint(num_points);

  std::vector<float> points_x;
  std::vector<float> points_y;
  nh.getParam("robot_footprint_x", points_x);
  nh.getParam("robot_footprint_y", points_y);

  for (uint8_t i = 0; i < num_points; i++)
  {
    robot_footprint.AddPoint(points_x[i], points_y[i]);
  }

  float embedded_update_rate;
  nh.getParam("embeddded_update_rate", embedded_update_rate);

  // Process incoming state messages at 2 times the update rate
  ros::Rate loop_rate(1 / (embedded_update_rate / 2));  // (Hz)

  InitEkfMsgs();

  while (ros::ok())
  {
    ros::spinOnce();
    footprint_pub.publish(robot_footprint.GetPolyStampedMsg());
    loop_rate.sleep();
  }
}

static void InitEkfMsgs(void)
{
  mpu.header.frame_id = "base_link";
  fxos.header.frame_id = "base_link";

  odom.header.frame_id = "odom";
  odom.child_frame_id = "base_link";

  mpu.angular_velocity_covariance[ZZ] = MPU_GZ_VARIANCE;

  mpu.linear_acceleration_covariance[XX] = MPU_AX_VARIANCE;
  fxos.linear_acceleration_covariance[XX] = FXOS_AX_VARIANCE;

  mpu.linear_acceleration_covariance[YY] = MPU_AY_VARIANCE;
  fxos.linear_acceleration_covariance[YY] = FXOS_AY_VARIANCE;
}

static void InitPoseCallBack(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& msg)
{
  x = msg->pose.pose.position.x;
  y = msg->pose.pose.position.y;

  tf::Quaternion q(msg->pose.pose.orientation.x, msg->pose.pose.orientation.y,
                   msg->pose.pose.orientation.z, msg->pose.pose.orientation.w);
  tf::Matrix3x3 m(q);
  double roll, pitch, yaw;
  m.getRPY(roll, pitch, yaw);
  th = yaw;

  robot_localization::SetPose set_pose;

  set_pose.request.pose.header.stamp = ros::Time::now();
  set_pose.request.pose.header.frame_id = "odom";
  for (size_t ind = 0; ind < 36; ind += 7)
  {
    set_pose.request.pose.pose.covariance[ind] = 1e-9;
  }
  set_pose.request.pose.pose.pose.position.x = x;
  set_pose.request.pose.pose.pose.position.y = y;
  set_pose.request.pose.pose.pose.orientation = tf::createQuaternionMsgFromYaw(th);

  reset_ekf_pose.call(set_pose);

  current_time = ros::Time::now();
  PublishOdometry();
}

static void StateMsgUpdateCallBack(const robo_car_ros_if::state::ConstPtr& msg)
{
  static double zero_yaw;
  static bool init = false;
  double yaw, dt;

  geometry_msgs::Quaternion fxos_quat;

  // Compute time between state messages
  current_time = ros::Time::now();
  dt = (current_time - last_time).toSec();
  last_time = current_time;

  // Compute the odometry
  const double vr = msg->r_wheel_fb * WHEEL_RADIUS;  // Right wheel translational velocity
  const double vl = msg->l_wheel_fb * WHEEL_RADIUS;  // Left wheel translational velocity
  const double th_dot = (vr - vl) / WHEEL_BASE;  // Robot angular velocity

  x_dot = (vr + vl) / 2;  // Robot translational velocity

  double delta_x = (x_dot * cos(th)) * dt;
  double delta_y = (x_dot * sin(th)) * dt;
  double delta_th = th_dot * dt;

  x += delta_x;
  y += delta_y;
  th += delta_th;

  PublishOdometry();

  mpu.header.stamp = current_time;
  mpu.angular_velocity.x = msg->mpu_gx;
  mpu.angular_velocity.y = msg->mpu_gy;
  mpu.angular_velocity.z = msg->mpu_gz;
  mpu.linear_acceleration.x = msg->mpu_ax;
  mpu.linear_acceleration.y = msg->mpu_ay;
  mpu.linear_acceleration.z = msg->mpu_az;
  imu_mpu_pub.publish(mpu);

  fxos.header.stamp = current_time;
  fxos.linear_acceleration.x = msg->fxos_ax;
  fxos.linear_acceleration.y = msg->fxos_ay;
  fxos.linear_acceleration.z = msg->fxos_az;

  imu_fxos_pub.publish(fxos);

  if (!init)
  {
    zero_yaw = atan2(msg->fxos_mx, msg->fxos_my);
    init = true;
  }

  yaw = atan2(msg->fxos_mx, msg->fxos_my) - zero_yaw;
  fxos.orientation = tf::createQuaternionMsgFromYaw(yaw);
}

static void PublishOdometry(void)
{
  geometry_msgs::Quaternion odom_quat;

  // Since all odometry is 6DOF we'll need a quaternion created from yaw
  odom_quat = tf::createQuaternionMsgFromYaw(th);

  odom.header.stamp = current_time;

  odom.pose.pose.position.x = x;
  odom.pose.pose.position.y = y;
  odom.pose.pose.position.z = 0.0;
  odom.pose.pose.orientation = odom_quat;

  odom.twist.twist.linear.x = x_dot;
  odom.twist.twist.linear.y = 0;
  odom.twist.twist.angular.z = th_dot;

  odom_pub.publish(odom);
}
