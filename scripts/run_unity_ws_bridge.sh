#!/usr/bin/env bash
set -eo pipefail

# Disable setup file tracing output.
unset AMENT_TRACE_SETUP_FILES

# Source ROS 2 and workspace overlays
source /opt/ros/${ROS_DISTRO:-humble}/setup.bash
source /home/user/workspace/kros/ros_ws/install/setup.bash

ros2 run unity_ws_bridge unity_ws_bridge_node --ros-args \
  -p bind_address:=0.0.0.0 \
  -p port:=8767 \
  -p scan_topic:=/scan \
  -p default_frame_id:=ms200k
