#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include <thread>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <memory>
#include <chrono>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Unity LidarSend magic: 0x4C494452 ("LIDR")
static constexpr uint32_t MAGIC_LIDR = 0x4C494452;

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

class UnityWsLidarBridge : public rclcpp::Node
{
public:
  UnityWsLidarBridge()
  : Node("unity_ws_lidar_bridge")
  {
    declare_parameter<std::string>("bind_address", "0.0.0.0");
    declare_parameter<int>("port", 8765);
    declare_parameter<std::string>("scan_topic", "/scan");
    declare_parameter<std::string>("default_frame_id", "ms200k");
    declare_parameter<bool>("use_ros_now_for_stamp", false);
    declare_parameter<int>("accept_poll_ms", 100);

    bind_address_ = get_parameter("bind_address").as_string();
    port_ = get_parameter("port").as_int();
    scan_topic_ = get_parameter("scan_topic").as_string();
    default_frame_id_ = get_parameter("default_frame_id").as_string();
    use_ros_now_for_stamp_ = get_parameter("use_ros_now_for_stamp").as_bool();
    accept_poll_ms_ = std::max(10, static_cast<int>(get_parameter("accept_poll_ms").as_int()));

    pub_scan_ = create_publisher<sensor_msgs::msg::LaserScan>(scan_topic_, rclcpp::SensorDataQoS());

    // Ensure shutdown() is called when Ctrl+C triggers ROS shutdown
    rclcpp::on_shutdown([this]() { shutdown(); });

    server_thread_ = std::thread([this]() { run_server(); });

    RCLCPP_INFO(get_logger(),
      "WebSocket server: ws://%s:%d  publishing: %s (accept_poll=%dms, use_ros_now_for_stamp=%s)",
      bind_address_.c_str(), port_, scan_topic_.c_str(), accept_poll_ms_,
      use_ros_now_for_stamp_ ? "true" : "false");
  }

  ~UnityWsLidarBridge() override
  {
    shutdown();
    if (server_thread_.joinable()) server_thread_.join();
  }

private:
  void shutdown()
  {
    if (shutting_down_.exchange(true)) return;

    // Close acceptor so accept loop can exit
    try {
      if (acceptor_ && acceptor_->is_open()) {
        boost::system::error_code ec;
        acceptor_->cancel(ec);
        acceptor_->close(ec);
      }
    } catch (...) {}

    // Close all active sessions so their read loops can exit
    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      for (auto& ws : sessions_) {
        if (!ws) continue;
        boost::system::error_code ec;

        // Cancel any pending I/O and close the underlying socket
        ws->next_layer().cancel(ec);
        ws->next_layer().shutdown(tcp::socket::shutdown_both, ec);
        ws->next_layer().close(ec);

        // Try websocket close handshake too (best effort)
        ws->close(websocket::close_code::normal, ec);
      }
      sessions_.clear();
    }

    // Stop io_context
    try { ioc_.stop(); } catch (...) {}
  }

  void run_server()
  {
    try {
      auto addr = net::ip::make_address(bind_address_);
      tcp::endpoint endpoint{addr, static_cast<unsigned short>(port_)};
      acceptor_ = std::make_unique<tcp::acceptor>(ioc_, endpoint);

      // Make accept non-blocking so Ctrl+C can stop quickly
      boost::system::error_code ec;
      acceptor_->non_blocking(true, ec);

      for (;;) {
        if (shutting_down_ || !rclcpp::ok()) break;

        tcp::socket socket{ioc_};
        acceptor_->accept(socket, ec);

        if (ec) {
          if (ec == net::error::would_block || ec == net::error::try_again) {
            std::this_thread::sleep_for(std::chrono::milliseconds(accept_poll_ms_));
            continue;
          }
          if (shutting_down_ || !rclcpp::ok()) break;

          RCLCPP_WARN(get_logger(), "Accept error: %s", ec.message().c_str());
          std::this_thread::sleep_for(std::chrono::milliseconds(accept_poll_ms_));
          continue;
        }

        std::thread(&UnityWsLidarBridge::handle_session, this, std::move(socket)).detach();
      }
    } catch (const std::exception& e) {
      if (!shutting_down_) {
        RCLCPP_ERROR(get_logger(), "Server error: %s", e.what());
      }
    }
  }

  void handle_session(tcp::socket socket)
  {
    std::shared_ptr<websocket::stream<tcp::socket>> ws;
    try {
      ws = std::make_shared<websocket::stream<tcp::socket>>(std::move(socket));

      {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.push_back(ws);
      }

      ws->accept();
      RCLCPP_INFO(get_logger(), "Unity client connected");

      for (;;) {
        if (shutting_down_ || !rclcpp::ok()) break;

        beast::flat_buffer buffer;
        boost::system::error_code ec;

        ws->read(buffer, ec);

        if (ec) {
          // Timeout or abort: just loop and re-check shutdown flags
          if (ec == beast::error::timeout ||
              ec == net::error::operation_aborted)
          {
            continue;
          }

          // Expected close cases
          if (ec == websocket::error::closed ||
              ec == net::error::eof)
          {
            break;
          }

          // Other errors
          throw boost::system::system_error(ec);
        }

        if (!ws->got_binary()) {
          // optional: handle text control messages here
          continue;
        }

        auto b = buffer.data();
        const auto* p = static_cast<const uint8_t*>(b.data());
        const std::size_t len = b.size();

        if (!parse_and_publish_lidar(p, len)) {
          RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000,
                               "Dropped malformed/unknown binary frame (len=%zu)", len);
        }
      }
    } catch (const std::exception& e) {
      if (!shutting_down_) {
        RCLCPP_WARN(get_logger(), "Session ended: %s", e.what());
      }
    }

    if (!shutting_down_) {
      RCLCPP_INFO(get_logger(), "Unity client disconnected");
    }

    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      if (ws) {
        sessions_.erase(
          std::remove(sessions_.begin(), sessions_.end(), ws),
          sessions_.end());
      }
    }
  }

  bool parse_and_publish_lidar(const uint8_t* p, std::size_t len)
  {
    if (len < 12) return false;

    const uint32_t magic = read_u32_le(p + 0);
    if (magic != MAGIC_LIDR) return false;

    const uint16_t version = read_u16_le(p + 4);
    // const uint16_t flags = read_u16_le(p + 6);  // reserved
    const uint32_t count = read_u32_le(p + 8);

    std::size_t header_size = 0;

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

    if (version >= 2) {
      // v2 header fixed size: 48 bytes
      header_size = 48;
      if (len < header_size) return false;

      stamp_sec     = read_i32_le(p + 12);
      stamp_nanosec = read_u32_le(p + 16);

      angle_min      = read_f32_le(p + 20);
      angle_max      = read_f32_le(p + 24);
      angle_inc      = read_f32_le(p + 28);
      time_increment = read_f32_le(p + 32);
      scan_time      = read_f32_le(p + 36);
      range_min      = read_f32_le(p + 40);
      range_max      = read_f32_le(p + 44);

    } else if (version == 1) {
      // Legacy v1 header: 44 bytes
      header_size = 44;
      if (len < header_size) return false;

      // float unity_stamp = read_f32_le(p + 12);  // legacy - ignore

      angle_min      = read_f32_le(p + 16);
      angle_max      = read_f32_le(p + 20);
      angle_inc      = read_f32_le(p + 24);
      time_increment = read_f32_le(p + 28);
      scan_time      = read_f32_le(p + 32);
      range_min      = read_f32_le(p + 36);
      range_max      = read_f32_le(p + 40);

      // No stamp in v1 (or it's float unity time). We'll use ROS now for v1.
      stamp_sec = 0;
      stamp_nanosec = 0;
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

    sensor_msgs::msg::LaserScan msg;
    msg.header.frame_id = default_frame_id_;

    // Stamp policy:
    // - For v2, prefer Unity-provided (SimClock-based) stamp unless user forces ROS now.
    // - For v1, always use ROS now.
    if (version >= 2 && !use_ros_now_for_stamp_) {
      msg.header.stamp.sec = stamp_sec;
      msg.header.stamp.nanosec = stamp_nanosec;
    } else {
      msg.header.stamp = this->now();
    }

    msg.angle_min = angle_min;
    msg.angle_max = angle_max;
    msg.angle_increment = angle_inc;

    // Safety: reconstruct angle_max if degenerate
    if (count > 1 && msg.angle_increment > 0.0f &&
        std::fabs(msg.angle_max - msg.angle_min) < 1e-6f) {
      msg.angle_max = msg.angle_min + msg.angle_increment * static_cast<float>(count - 1);
    }

    msg.time_increment = time_increment;
    msg.scan_time = scan_time;

    msg.range_min = range_min;
    msg.range_max = range_max;

    msg.ranges = std::move(ranges);
    msg.intensities.clear();

    pub_scan_->publish(msg);
    return true;
  }

private:
  rclcpp::Publisher<sensor_msgs::msg::LaserScan>::SharedPtr pub_scan_;

  std::string bind_address_;
  int port_;
  std::string scan_topic_;
  std::string default_frame_id_;
  bool use_ros_now_for_stamp_;

  int accept_poll_ms_;

  net::io_context ioc_;
  std::unique_ptr<tcp::acceptor> acceptor_;
  std::thread server_thread_;

  std::atomic<bool> shutting_down_{false};

  std::mutex sessions_mutex_;
  std::vector<std::shared_ptr<websocket::stream<tcp::socket>>> sessions_;
};

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<UnityWsLidarBridge>());
  rclcpp::shutdown();
  return 0;
}
