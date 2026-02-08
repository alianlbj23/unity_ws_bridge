#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>
#include <std_msgs/msg/float32_multi_array.hpp>

#include "unity_ws_bridge/lidar_parser.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include <thread>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <memory>
#include <chrono>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class UnityWsBridge : public rclcpp::Node
{
public:
  UnityWsBridge()
  : Node("unity_ws_bridge")
  {
    declare_parameter<std::string>("bind_address", "0.0.0.0");
    declare_parameter<int>("port", 8765);
    declare_parameter<std::string>("scan_topic", "/scan");
    declare_parameter<std::string>("ros_float_array_topic", "/car_C_front_wheel");
    declare_parameter<std::string>("default_frame_id", "ms200k");
    declare_parameter<bool>("use_ros_now_for_stamp", false);
    declare_parameter<int>("accept_poll_ms", 100);

    bind_address_ = get_parameter("bind_address").as_string();
    port_ = get_parameter("port").as_int();
    scan_topic_ = get_parameter("scan_topic").as_string();
    ros_float_array_topic_ = get_parameter("ros_float_array_topic").as_string();
    default_frame_id_ = get_parameter("default_frame_id").as_string();
    use_ros_now_for_stamp_ = get_parameter("use_ros_now_for_stamp").as_bool();
    accept_poll_ms_ = std::max(10, static_cast<int>(get_parameter("accept_poll_ms").as_int()));

    pub_scan_ = create_publisher<sensor_msgs::msg::LaserScan>(scan_topic_, rclcpp::SensorDataQoS());
    sub_float_array_ = create_subscription<std_msgs::msg::Float32MultiArray>(
      ros_float_array_topic_, rclcpp::SensorDataQoS(),
      [this](const std_msgs::msg::Float32MultiArray::SharedPtr msg) {
        forward_float32_array_to_unity(*msg);
      });

    // Ensure shutdown() is called when Ctrl+C triggers ROS shutdown
    rclcpp::on_shutdown([this]() { shutdown(); });

    server_thread_ = std::thread([this]() { run_server(); });

    RCLCPP_INFO(get_logger(),
      "WebSocket server: ws://%s:%d  publishing: %s, forwarding: %s (accept_poll=%dms, use_ros_now_for_stamp=%s)",
      bind_address_.c_str(), port_, scan_topic_.c_str(), ros_float_array_topic_.c_str(), accept_poll_ms_,
      use_ros_now_for_stamp_ ? "true" : "false");
  }

  ~UnityWsBridge() override
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

        std::thread(&UnityWsBridge::handle_session, this, std::move(socket)).detach();
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
    sensor_msgs::msg::LaserScan msg;
    unity_ws_bridge::LidarParseConfig cfg;
    cfg.frame_id = default_frame_id_;
    cfg.use_ros_now_for_stamp = use_ros_now_for_stamp_;
    cfg.ros_now = this->now();

    if (!unity_ws_bridge::parse_lidar_frame(p, len, cfg, msg)) {
      return false;
    }

    pub_scan_->publish(msg);
    return true;
  }

  static inline void write_f32_le(uint8_t* dst, float v)
  {
    uint32_t u;
    std::memcpy(&u, &v, sizeof(uint32_t));
    dst[0] = static_cast<uint8_t>(u & 0xFF);
    dst[1] = static_cast<uint8_t>((u >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((u >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((u >> 24) & 0xFF);
  }

  void forward_float32_array_to_unity(const std_msgs::msg::Float32MultiArray& msg)
  {
    std::vector<std::shared_ptr<websocket::stream<tcp::socket>>> sessions_copy;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      if (sessions_.empty()) return;
      sessions_copy = sessions_;
    }

    const std::size_t count = msg.data.size();
    std::vector<uint8_t> payload(count * 4);
    for (std::size_t i = 0; i < count; ++i) {
      write_f32_le(payload.data() + i * 4, msg.data[i]);
    }

    std::lock_guard<std::mutex> write_lock(ws_write_mutex_);
    for (auto& ws : sessions_copy) {
      if (!ws) continue;
      boost::system::error_code ec;
      ws->binary(true);
      ws->write(net::buffer(payload), ec);
      if (ec) {
        RCLCPP_DEBUG(get_logger(), "Unity send error: %s", ec.message().c_str());
      }
    }
  }

private:
  rclcpp::Publisher<sensor_msgs::msg::LaserScan>::SharedPtr pub_scan_;
  rclcpp::Subscription<std_msgs::msg::Float32MultiArray>::SharedPtr sub_float_array_;

  std::string bind_address_;
  int port_;
  std::string scan_topic_;
  std::string ros_float_array_topic_;
  std::string default_frame_id_;
  bool use_ros_now_for_stamp_;

  int accept_poll_ms_;

  net::io_context ioc_;
  std::unique_ptr<tcp::acceptor> acceptor_;
  std::thread server_thread_;

  std::atomic<bool> shutting_down_{false};

  std::mutex sessions_mutex_;
  std::mutex ws_write_mutex_;
  std::vector<std::shared_ptr<websocket::stream<tcp::socket>>> sessions_;
};

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<UnityWsBridge>());
  rclcpp::shutdown();
  return 0;
}
