#!/usr/bin/env bash
set -euo pipefail

source_if_present() {
  if [[ -f "$1" ]]; then
    # shellcheck disable=SC1090
    set +u
    source "$1"
    set -u
  fi
}

# ROS 2 launch 与 Gazebo 都会在用户命令启动前创建运行时状态。
# 即使镜像以只读方式运行、源码树以只读方式挂载，
# 这些路径也要保留在可写的 tmpfs 上。
mkdir -p "${ROS_LOG_DIR:-/tmp/ros-log}" "${GZ_LOG_PATH:-/tmp/gz-log}" "${XDG_CACHE_HOME:-/tmp/cache}"

# source 顺序是对外承诺的开发容器保证。
source_if_present /opt/ros/jazzy/setup.bash
source_if_present /opt/workbench_ws/install/setup.bash
source_if_present /workspace/install/setup.bash

export WORKBENCH_CONTAINER_PROFILE="${WORKBENCH_CONTAINER_PROFILE:-dashboard}"
case "$WORKBENCH_CONTAINER_PROFILE" in
  dashboard|ros-sim|gz-gui-x11|gz-gui-wayland|mujoco-gpu|hardware-shell) ;;
  *) echo "invalid WORKBENCH_CONTAINER_PROFILE=$WORKBENCH_CONTAINER_PROFILE" >&2; exit 2 ;;
esac

if [[ "$WORKBENCH_CONTAINER_PROFILE" != "dashboard" ]]; then
  /usr/local/bin/workbench-container-doctor --profile "$WORKBENCH_CONTAINER_PROFILE"
fi

if [[ "$WORKBENCH_CONTAINER_PROFILE" == "hardware-shell" ]]; then
  /usr/local/bin/workbench-dds-config --output /tmp/workbench-fastdds.xml
  export FASTRTPS_DEFAULT_PROFILES_FILE=/tmp/workbench-fastdds.xml
fi

exec "$@"
