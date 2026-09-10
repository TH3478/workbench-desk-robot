# BSP 启动与恢复清单

本清单是所选 Jetson 原型板的闸门。依赖最终载板的值保持 `TBD`，直到从其厂商手册与原理图取得出处。

## 必需记录

- JetPack/L4T 与 Linux 内核版本、编译器版本与配置 hash
- 启动介质镜像 hash、UEFI/引导加载器配置与内核命令行
- 设备树源码与编译后的 DTB hash
- rootfs 清单、systemd 单元列表与固件捆绑包 hash
- 恢复镜像、回滚流程与串口控制台记录

机器可读的事实源是 `bsp/image/build-inputs.yaml`。构建前运行 `python bsp/validation/validate_image_inputs.py`。入库清单刻意保持 `inputs_unresolved` 与 `build_ready: false`，直到每个版本、来源、仓库输入与摘要冻结。生成的 hash 从不证明镜像在硬件上启动过。

## 启动调试顺序

1. 在运动供电隔离的情况下启动；验证控制台、存储、以太网与 USB。
2. 验证 Linux 看门狗与重启恢复，不使能执行器。
3. 在无运动负载的情况下调起 `can0`；选定物理适配器后记录控制器、比特率与错误计数器。
4. 先发现 `MCU-SAFETY`，并证明硬件抑制保持有效。
5. 发现 `MCU-BASE`、`ARM-L-CTRL`、`ARM-R-CTRL`、`TOOL-L-CTRL` 与 `TOOL-R-CTRL`；记录六路心跳与启动 ID。
6. 在受控条件下演练 STOP、节点复位、Linux 重启、bus-off 恢复与手动复位。

不得从仅主机或 `wbcan` 运行中声称任何执行器使能、物理成功、EMC 通过或硬实时结果。
