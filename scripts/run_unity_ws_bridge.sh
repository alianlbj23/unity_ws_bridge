#!/usr/bin/env bash
set -eo pipefail

# Disable setup file tracing output.
unset AMENT_TRACE_SETUP_FILES

# Go to workspace root (three levels up from this script)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WS_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
cd "${WS_DIR}"

# Build and source workspace
colcon build --packages-select unity_ws_bridge
source "${WS_DIR}/install/setup.bash"

ros2 run unity_ws_bridge unity_ws_bridge_node --ros-args \
  -p bind_address:=0.0.0.0 \
  -p port:=8765 \
  -p scan_topic:=/scan \
  -p default_frame_id:=ms200k
