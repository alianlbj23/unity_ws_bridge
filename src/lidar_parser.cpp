#include "unity_ws_bridge/lidar_parser.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace unity_ws_bridge {

// Unity LidarSend magic: 0x4C494452 ("LIDR")
static constexpr uint32_t kMagicLidr = 0x4C494452;

// Little-endian readers (portable)
static inline uint16_t read_u16_le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
static inline uint32_t read_u32_le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
static inline int32_t read_i32_le(const uint8_t* p) {
  return static_cast<int32_t>(
    (uint32_t)p[0] |
    ((uint32_t)p[1] << 8) |
    ((uint32_t)p[2] << 16) |
    ((uint32_t)p[3] << 24)
  );
}
static inline float read_f32_le(const uint8_t* p) {
  uint32_t u = read_u32_le(p);
  float f;
  std::memcpy(&f, &u, sizeof(float));
  return f;
}

bool parse_lidar_frame(const uint8_t* p,
                       std::size_t len,
                       const LidarParseConfig& config,
                       sensor_msgs::msg::LaserScan& out_msg)
{
  if (len < 10) return false;

  const uint32_t magic = read_u32_le(p + 0);
  if (magic != kMagicLidr) return false;

  const uint16_t version = read_u16_le(p + 4);

  std::size_t header_size = 0;
  uint32_t count = 0;
  bool has_flags = false;

  // Parsed fields
  int32_t stamp_sec = 0;
  uint32_t stamp_nanosec = 0;

  float angle_min = 0.0f;
  float angle_max = 0.0f;
  float angle_inc = 0.0f;
  float time_increment = 0.0f;
  float scan_time = 0.0f;
  float range_min = 0.0f;
  float range_max = 0.0f;

  if (version >= 2 || version == 1) {
    bool match_flags = false;
    bool match_noflags = false;
    std::size_t expected_flags = 0;
    std::size_t expected_noflags = 0;

    if (len >= 12) {
      const uint32_t count_flags = read_u32_le(p + 8);
      const std::size_t header_flags = (version >= 2) ? 48 : 44;
      expected_flags = header_flags + static_cast<std::size_t>(count_flags) * 4;
      if (len >= expected_flags) {
        match_flags = true;
      }
    }

    if (len >= 10) {
      const uint32_t count_noflags = read_u32_le(p + 6);
      const std::size_t header_noflags = (version >= 2) ? 46 : 42;
      expected_noflags = header_noflags + static_cast<std::size_t>(count_noflags) * 4;
      if (len >= expected_noflags) {
        match_noflags = true;
      }
    }

    if (match_noflags && (!match_flags || (expected_noflags == len && expected_flags != len))) {
      has_flags = false;
      count = read_u32_le(p + 6);
      header_size = (version >= 2) ? 46 : 42;
    } else if (match_flags) {
      has_flags = true;
      count = read_u32_le(p + 8);
      header_size = (version >= 2) ? 48 : 44;
    } else {
      return false;
    }

    const std::size_t base = has_flags ? 12 : 10;

    if (version >= 2) {
      stamp_sec     = read_i32_le(p + base + 0);
      stamp_nanosec = read_u32_le(p + base + 4);

      angle_min      = read_f32_le(p + base + 8);
      angle_max      = read_f32_le(p + base + 12);
      angle_inc      = read_f32_le(p + base + 16);
      time_increment = read_f32_le(p + base + 20);
      scan_time      = read_f32_le(p + base + 24);
      range_min      = read_f32_le(p + base + 28);
      range_max      = read_f32_le(p + base + 32);
    } else {
      // Legacy v1 header contains a float unity_stamp (ignored) after count
      angle_min      = read_f32_le(p + base + 4);
      angle_max      = read_f32_le(p + base + 8);
      angle_inc      = read_f32_le(p + base + 12);
      time_increment = read_f32_le(p + base + 16);
      scan_time      = read_f32_le(p + base + 20);
      range_min      = read_f32_le(p + base + 24);
      range_max      = read_f32_le(p + base + 28);

      // No stamp in v1 (or it's float unity time). We'll use ROS now for v1.
      stamp_sec = 0;
      stamp_nanosec = 0;
    }
  } else {
    return false;
  }

  const std::size_t expected = header_size + static_cast<std::size_t>(count) * 4;
  if (len < expected) return false;

  std::vector<float> ranges(count);
  const uint8_t* pr = p + header_size;
  for (uint32_t i = 0; i < count; i++) {
    ranges[i] = read_f32_le(pr + i * 4);
  }

  out_msg = sensor_msgs::msg::LaserScan();
  out_msg.header.frame_id = config.frame_id;

  // Stamp policy:
  // - For v2, prefer Unity-provided (SimClock-based) stamp unless user forces ROS now.
  // - For v1, always use ROS now.
  if (version >= 2 && !config.use_ros_now_for_stamp) {
    out_msg.header.stamp.sec = stamp_sec;
    out_msg.header.stamp.nanosec = stamp_nanosec;
  } else {
    out_msg.header.stamp = config.ros_now;
  }

  out_msg.angle_min = angle_min;
  out_msg.angle_max = angle_max;
  out_msg.angle_increment = angle_inc;

  // Safety: reconstruct angle_max if degenerate
  if (count > 1 && out_msg.angle_increment > 0.0f &&
      std::fabs(out_msg.angle_max - out_msg.angle_min) < 1e-6f) {
    out_msg.angle_max = out_msg.angle_min + out_msg.angle_increment * static_cast<float>(count - 1);
  }

  out_msg.time_increment = time_increment;
  out_msg.scan_time = scan_time;

  out_msg.range_min = range_min;
  out_msg.range_max = range_max;

  out_msg.ranges = std::move(ranges);
  out_msg.intensities.clear();

  return true;
}

}  // namespace unity_ws_bridge
