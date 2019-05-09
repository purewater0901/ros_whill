/*
MIT License

Copyright (c) 2018 WHILL inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <string.h>
#include <stdint.h>
#include <vector>

#include "ros/ros.h"
#include "sensor_msgs/Joy.h"
#include "sensor_msgs/JointState.h"
#include "sensor_msgs/Imu.h"
#include "sensor_msgs/BatteryState.h"
#include "nav_msgs/Odometry.h"

#include "std_srvs/Empty.h"

#include "tf/transform_broadcaster.h"

#include "whill/WHILL.h"   
#include "serial/serial.h"  // wjwood/Serial (ros-melodic-serial)

#include "utils/rotation_tools.h"
#include "odom.h"

// #include "./includes/subscriber.hpp"
// #include "./includes/services.hpp"

WHILL *whill = nullptr;
Odometry odom;


// Global Parameters
int interval = 0;          // WHILL Data frequency
bool publish_tf = true;    // Enable publishing Odometry TF

//
// ROS Objects
//

// Publishers
ros::Publisher ros_joystick_state_publisher;
ros::Publisher ros_jointstate_publisher;
ros::Publisher ros_imu_publisher;
ros::Publisher ros_battery_state_publisher;
ros::Publisher ros_odom_publisher;

// TF Broadcaster
tf::TransformBroadcaster *odom_broadcaster = nullptr;

// 
// ROS Callbacks
//
void ros_joystick_callback(const sensor_msgs::Joy::ConstPtr &joy)
{
    // Transform [-1.0,1.0] to [-100,100]
    int joy_x = -joy->axes[0] * 100.0f;
    int joy_y = joy->axes[1] * 100.0f;

    // value check
    if (joy_y < -100)
        joy_y = -100;
    if (joy_y > 100)
        joy_y = 100;
    if (joy_x < -100)
        joy_x = -100;
    if (joy_x > 100)
        joy_x = 100;

    if (whill)
    {
        whill->setJoystick(joy_x, joy_y);
    }
}

void ros_cmd_vel_callback(const geometry_msgs::Twist::ConstPtr &cmd_vel)
{
    if (whill)
    {
        whill->setSpeed(cmd_vel->linear.x, cmd_vel->angular.z);
    }
}

bool ros_srv_odom_clear_callback(std_srvs::Empty::Request &req, std_srvs::Empty::Response &res)
{
    ROS_INFO("Clear Odometry");
    odom.reset();
    return true;
}

//
//  UART Interface
//
serial::Serial *ser = nullptr;

int serialRead(std::vector<uint8_t> &data)
{
    if (ser && ser->isOpen())
    {
        return ser->read(data, 30); // How many bytes read in one time.
    }
    return 0;
}

int serialWrite(std::vector<uint8_t> &data)
{
    if (ser && ser->isOpen())
    {
        return ser->write(data);
    }
    return 0;
}



//
// WHILL
//
void whill_callback_data1(WHILL *caller)
{

    // This function is called when receive Joy/Accelerometer/Gyro,etc.

    ros::Time currentTime = ros::Time::now();

    // Joy
    sensor_msgs::Joy joy;
    joy.header.stamp = currentTime;
    joy.axes.resize(2);
    joy.axes[0] = -caller->joy.x / 100.0f; //X
    joy.axes[1] = caller->joy.y / 100.0f; //Y
    ros_joystick_state_publisher.publish(joy);


    // IMU
    sensor_msgs::Imu imu;
    imu.header.frame_id = "imu";

    imu.orientation_covariance[0] = -1; // Orientation is unknown

    imu.angular_velocity.x = caller->gyro.x / 180 * M_PI; // deg per sec to rad/s
    imu.angular_velocity.y = caller->gyro.y / 180 * M_PI; // deg per sec to rad/s
    imu.angular_velocity.z = caller->gyro.z / 180 * M_PI; // deg per sec to rad/s

    imu.linear_acceleration.x = caller->accelerometer.x * 9.80665; // G to m/ss
    imu.linear_acceleration.y = caller->accelerometer.y * 9.80665; // G to m/ssnav_msgs::Odometry odom_msg = odom.getROSOdometry();
    imu.linear_acceleration.z = caller->accelerometer.z * 9.80665; // G to m/ss
    ros_imu_publisher.publish(imu);


    // Battery
    sensor_msgs::BatteryState batteryState;
    batteryState.voltage = 25.2;                           //[V] Spec voltage, since raw voltage is not provided.
    batteryState.current = -caller->battery.current / 1000.0f; // mA -> A
    batteryState.charge = std::numeric_limits<float>::quiet_NaN();
    batteryState.design_capacity = 10.04;                 //[Ah]
    batteryState.percentage = caller->battery.level / 100.0f; // Percentage
    batteryState.power_supply_status = sensor_msgs::BatteryState::POWER_SUPPLY_STATUS_UNKNOWN;
    batteryState.power_supply_health = sensor_msgs::BatteryState::POWER_SUPPLY_HEALTH_UNKNOWN;
    batteryState.power_supply_technology = sensor_msgs::BatteryState::POWER_SUPPLY_TECHNOLOGY_LION;
    batteryState.present = true;
    batteryState.location = "0";
    ros_battery_state_publisher.publish(batteryState);


    // JointState
    sensor_msgs::JointState jointState;
    jointState.name.resize(2);
    jointState.position.resize(2);
    jointState.velocity.resize(2);

    jointState.name[0] = "leftWheel";
    jointState.position[0] = caller->left_motor.angle; //Rad
    jointState.name[1] = "rightWheel";
    jointState.position[1] = caller->right_motor.angle; //Rad

    static double joint_past[2] = {0.0f, 0.0f};
    if(caller->_interval == -1){
        // Standard, Constant specified time intervel
        jointState.velocity[0] = rad_diff(joint_past[0], jointState.position[0]) / (double(interval) / 1000.0f); // Rad/sec
        jointState.velocity[1] = rad_diff(joint_past[1], jointState.position[1]) / (double(interval) / 1000.0f); // Rad/sec
    }else if(caller->_interval == 0){
        // Experimental, Motor Control Disabled (= Brake Locked)
        jointState.velocity[0] = 0.0f;
        jointState.velocity[1] = 0.0f;
    }else{
        // Experimental, Under motor controlling
        jointState.velocity[0] = rad_diff(joint_past[0], jointState.position[0]) / (double(caller->_interval) / 1000.0f);  // Rad/sec
        jointState.velocity[1] = rad_diff(joint_past[1], jointState.position[1]) / (double(caller->_interval) / 1000.0f);  // Rad/sec
    }
    joint_past[0] = jointState.position[0];
    joint_past[1] = jointState.position[1];

    ros_jointstate_publisher.publish(jointState);


    // Odometory
    if(caller->_interval == -1){
        // Standard
        odom.update(jointState, interval / 1000.0f);
    }else if(caller->_interval >= 0){
        // Experimental
        odom.update(jointState, caller->_interval / 1000.0f);
    }

    nav_msgs::Odometry odom_msg = odom.getROSOdometry();
    odom_msg.header.stamp = currentTime;
    odom_msg.header.frame_id = "odom";
    odom_msg.child_frame_id = "base_link";
    ros_odom_publisher.publish(odom_msg);


    // Odometory TF
    if (publish_tf)
    {
        geometry_msgs::TransformStamped odom_trans = odom.getROSTransformStamped();
        odom_trans.header.stamp = currentTime;
        odom_trans.header.frame_id = "odom";
        odom_trans.child_frame_id = "base_link";
        if (odom_broadcaster){
            odom_broadcaster->sendTransform(odom_trans);
        }
    }
}

void whill_callback_powered_on(WHILL *caller)
{
    // This function is called when powered on via setPower()
    ROS_INFO("power_on");
}


//
// Main
//
int main(int argc, char **argv)
{
    // ROS setup
    ros::init(argc, argv, "whill");
    ros::NodeHandle nh("~");


    // Services
    //set_power_service_service = nh.advertiseService("power/on", set_power_service_callback);
    ros::ServiceServer clear_odom_service        = nh.advertiseService("odom/clear", &ros_srv_odom_clear_callback);


    // Subscriber
    ros::Subscriber joystick_subscriber = nh.subscribe("controller/joy", 100, ros_joystick_callback);
    ros::Subscriber twist_subscriber = nh.subscribe("controller/cmd_vel", 100, ros_cmd_vel_callback);


    // Publishers
    ros_joystick_state_publisher = nh.advertise<sensor_msgs::Joy>("states/joy", 100);
    ros_jointstate_publisher = nh.advertise<sensor_msgs::JointState>("states/jointState", 100);
    ros_imu_publisher = nh.advertise<sensor_msgs::Imu>("states/imu", 100);
    ros_battery_state_publisher = nh.advertise<sensor_msgs::BatteryState>("states/batteryState", 100);
    ros_odom_publisher = nh.advertise<nav_msgs::Odometry>("odom", 100);


    // TF Broadcaster
    odom_broadcaster = new tf::TransformBroadcaster;


    // Parameters
    // WHILL Report Packet Interval
    nh.getParam("send_interval", interval);
    if (interval < 10)
    {
        ROS_WARN("Too short interval. Set interval > 10");
        nh.setParam("send_interval", 10);
        interval = 10;
    }
    ROS_INFO("param: send_interval=%d", interval);

    // Serial Port Device Name
    std::string serialport;
    nh.param<std::string>("serialport", serialport, "/dev/ttyUSB0");

    // Disable publishing odometry tf
    nh.param<bool>("publish_tf", publish_tf, true);

    // Enable Experimantal Topics
    bool activate_experimental_topics;
    nh.param<bool>("activate_experimental_topics", activate_experimental_topics, false);
    if (activate_experimental_topics)
    {
        ros::Subscriber control_cmd_vel_subscriber = nh.subscribe("controller/cmd_vel", 100, ros_cmd_vel_callback);
    }



    unsigned long baud = 38400;
    serial::Timeout timeout = serial::Timeout::simpleTimeout(0);
    timeout.write_timeout_multiplier = 5;  // Wait 5ms every bytes

    ser = new serial::Serial(serialport, baud, timeout);
    ser->flush();

    whill = new WHILL(serialRead, serialWrite);
    whill->register_callback(whill_callback_data1, WHILL::EVENT::CALLBACK_DATA1);
    whill->register_callback(whill_callback_powered_on, WHILL::EVENT::CALLBACK_POWER_ON);
    whill->begin(20); // ms



    ros::AsyncSpinner spinner(1);
    spinner.start();
    ros::Rate rate(100);

    while (ros::ok())
    {
        whill->refresh();
        rate.sleep();
    }

    spinner.stop();

    return 0;
}