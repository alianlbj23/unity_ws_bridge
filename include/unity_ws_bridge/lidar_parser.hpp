#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <rclcpp/time.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>

namespace unity_ws_bridge {

struct LidarParseConfig {
  std::string frame_id;
  bool use_ros_now_for_stamp = false;
  rclcpp::Time ros_now;
};

// Parse Unity LidarSend binary frame into LaserScan.
// Returns true on success, false on malformed/unknown frames.
bool parse_lidar_frame(const uint8_t* p,
                       std::size_t len,
                       const LidarParseConfig& config,
                       sensor_msgs::msg::LaserScan& out_msg);

}  // namespace unity_ws_bridge
