# USV Orangepi 控制系统详细设计

## 1. 设计目标与约束

### 1.1 系统目标
- 接收上位机任务与手控指令，完成稳定执行。
- 将上位机输入转换为推进器可执行命令，保证可控、可停、可恢复。
- 处理 SLAM 图像与定位结果，并在受限带宽下稳定回传。
- 在失联、异常输入、设备故障时自动进入安全模式。

### 1.2 运行环境
- 硬件: Orangepi 主控，推进器驱动板，IMU/GNSS/相机。
- 系统: Linux。
- 语言: C++17。
- 部署: systemd 管理多进程。

### 1.3 非功能约束
- 控制闭环频率: 50 到 100 Hz。
- 指令链路端到端延迟目标: 常态小于 60 ms。
- 图像链路策略: 控制链路优先，图像回传自适应降帧降码率。
- 进程崩溃恢复: 3 s 内自动重启并恢复到可控态。

## 2. 总体架构

采用 三层 五进程 架构:

- 接入与通信层
  - usv_gateway: 上位机连接、协议编解码、心跳、回传调度。
- 控制与任务层
  - usv_control: 状态机、控制算法、模式切换、安全策略。
- 设备与感知层
  - usv_thruster: 推进器驱动与反馈采集。
  - usv_slam: 图像采集、SLAM、编码与发布。
- 运行保障层
  - usv_supervisor: 健康监控、故障汇聚、降级决策、事件日志。

进程间通信采用本机 IPC 消息总线，统一二进制消息头，按主题发布订阅。

## 3. 模块职责

### 3.1 usv_gateway
- 管理 TCP 或 UDP 会话，支持断线重连。
- 将上位机协议转换为内部消息。
- 维护上位机心跳超时计时器。
- 将遥测、告警、图像元信息、SLAM 结果打包回传。
- 限流与优先级调度: 控制状态高优先级，图像低优先级。

### 3.2 usv_control
- 维护任务状态机与控制模式。
- 进行坐标转换和控制律计算。
- 进行限幅、斜率限制、安全裁剪。
- 输出左右推进器目标推力。
- 处理失联、传感器异常、执行器故障降级。

### 3.3 usv_thruster
- 接收目标推力并转换为 PWM 或电机控制命令。
- 读取推进器反馈: 电流、转速、温度、故障码。
- 执行本地保护: 超流、过温、通信超时时强制降功率或停车。

### 3.4 usv_slam
- 相机采集、预处理、SLAM 推理。
- 发布位姿、关键帧、质量评分。
- 进行图像压缩并输出多档码率。
- 接收带宽策略进行动态调参。

### 3.5 usv_supervisor
- 汇聚进程心跳、CPU/内存、队列积压、错误码。
- 下发系统级降级命令。
- 负责统一事件日志与故障快照。

## 4. 线程模型

### 4.1 usv_gateway 线程
- net_rx: 接收上位机数据。
- proto_parse: 协议解析。
- msg_pub: 发布内部命令。
- telemetry_mux: 回传消息合并与优先级调度。
- net_tx: 发送回上位机。

### 4.2 usv_control 线程
- control_loop: 固定周期 20 ms。
- state_machine: 模式管理与状态迁移。
- safety_guard: 安全裁剪与超时处理。

### 4.3 usv_slam 线程
- camera_capture: 采集线程。
- slam_worker: 算法线程。
- encoder: 压缩线程。
- uplink_adaptor: 带宽自适应线程。

### 4.4 并发规则
- 共享数据仅通过消息传递，避免跨线程共享可变结构。
- 必要共享变量采用单写多读并配合原子类型。
- 所有队列设置高水位线与丢弃策略，防止内存膨胀。

## 5. 消息总线与协议设计

### 5.1 内部消息头
固定头字段:
- magic: 2 字节
- version: 1 字节
- msg_type: 2 字节
- src: 1 字节
- dst: 1 字节
- seq: 4 字节
- timestamp_ms: 8 字节
- payload_len: 4 字节
- crc32: 4 字节

### 5.2 主题定义
- cmd.remote: 上位机控制命令
- cmd.mission: 任务指令
- state.nav: 导航状态
- state.thruster: 推进器状态
- alarm.system: 系统告警
- slam.pose: 位姿结果
- slam.image_meta: 图像元信息
- hb.process: 进程心跳

### 5.3 上位机命令模型
- SetMode
  - mode: MANUAL, AUTO, HOLD, EMERGENCY_STOP
- SetVelocity
  - surge_mps, yaw_rate_dps
- SetWaypoint
  - lat, lon, tolerance_m
- SetParam
  - key, value
- Ack
  - seq, result_code

### 5.4 遥测模型
- TelemetryBasic
  - mode, armed, battery_v, cpu_load, link_quality
- TelemetryNav
  - lat, lon, heading_deg, speed_mps, pose_quality
- TelemetryThruster
  - left_cmd, right_cmd, left_rpm, right_rpm, current_a
- Alarm
  - level, code, text, ts

## 6. 控制设计

### 6.1 模式定义
- INIT: 上电自检
- STANDBY: 待命
- MANUAL: 上位机手控
- AUTO: 航点或航线自主
- HOLD: 定点或低速保持
- SAFE: 降级安全航行
- ESTOP: 紧急停机

### 6.2 状态迁移核心条件
- INIT -> STANDBY: 自检通过且传感器就绪。
- 任意 -> ESTOP: 收到急停或触发硬故障。
- MANUAL/AUTO/HOLD -> SAFE: 失联超时或关键传感器失效。
- SAFE -> STANDBY: 链路恢复且人工确认。

### 6.3 控制律
- 外环: 航向和速度控制，输出目标角速度与推力。
- 内环: 左右推进器分配。
- 推进器分配公式:
  - left = clamp(F - K * yaw, min, max)
  - right = clamp(F + K * yaw, min, max)
- 加入斜率限制避免冲击:
  - u_t = clamp(u_t-1 + delta_max, target - delta_max, target + delta_max)

### 6.4 安全裁剪
- 速度上限、角速度上限、推力上限。
- 电池低压时自动降推力。
- 推进器过温或过流时单侧降额并触发告警。

## 7. SLAM 与图像回传设计

### 7.1 处理流程
- 采集原始帧。
- 做去畸变与时间同步。
- SLAM 推理输出 pose 和 quality。
- 编码器输出 H264 或 MJPEG。
- 回传策略器按链路质量动态调整 fps、分辨率、码率。

### 7.2 回传策略
- 优先级: 告警 > 基础遥测 > 位姿 > 图像。
- 弱网时动作:
  - 降帧: 15 fps -> 10 fps -> 5 fps。
  - 降分辨率: 720p -> 540p -> 360p。
  - 关键帧优先: 保留关键帧元数据。

### 7.3 延迟控制
- 图像编码队列长度上限 3。
- 超过上限丢弃最旧非关键帧。
- 回传目标端到端延迟小于 400 ms。

## 8. 故障处理与降级

### 8.1 故障分级
- FATAL: 必须急停。
- MAJOR: 进入 SAFE，限制速度。
- MINOR: 记录并告警，不中断任务。

### 8.2 典型故障动作
- 上位机失联 > 2 s: 切 SAFE。
- IMU 数据超时 > 200 ms: 切 SAFE 并限制转向。
- 推进器反馈异常持续 > 500 ms: 单侧降额，必要时 ESTOP。
- SLAM 质量持续低于阈值: 停止依赖视觉导航，退回惯导或手控。

### 8.3 看门狗
- 进程级: 每 200 ms 心跳。
- 系统级: supervisor 检测 3 次丢心跳触发重启。
- 硬件级: 可选独立硬件看门狗保底。

## 9. 配置与参数管理

### 9.1 配置分类
- static_config: 串口、设备 ID、主题映射。
- control_config: PID、限幅、斜率。
- uplink_config: 图像码率档位、优先级。
- safety_config: 超时阈值、故障动作。

### 9.2 热更新规则
- 支持运行时更新 control_config 和 uplink_config。
- 参数更新流程: 校验 -> 灰度生效 -> 回执 -> 持久化。
- 非法参数拒绝并回传错误码。

## 10. 日志、监控与追踪

### 10.1 日志规范
- 结构化日志字段: ts, level, module, code, msg, kv。
- 关键事件必须带 seq 和 mode。
- 日志分级: DEBUG, INFO, WARN, ERROR, FATAL。

### 10.2 指标
- 控制环周期抖动。
- 指令处理延迟分布。
- 图像回传帧率和端到端延迟。
- 进程重启次数与故障码计数。

### 10.3 追踪
- 关键消息沿链路携带 trace_id。
- 支持离线回放控制和感知数据。

## 11. 启停时序

### 11.1 启动顺序
1. supervisor 启动并加载配置。
2. thruster 启动并进入受控待命。
3. slam 启动并预热。
4. control 启动并等待依赖就绪。
5. gateway 启动并等待上位机连接。
6. 全模块自检通过进入 STANDBY。

### 11.2 关闭顺序
1. gateway 停止接收新任务并广播停机。
2. control 下发零推力。
3. thruster 确认停车。
4. slam 停止采集与编码。
5. supervisor 归档日志与状态。

## 12. 接口示例

### 12.1 控制命令接口
- 输入: SetVelocity(surge_mps, yaw_rate_dps)
- 输出: Ack(seq, result_code)
- 错误码:
  - 0 成功
  - 1001 参数越界
  - 1002 模式不允许
  - 1003 系统降级中

### 12.2 推进器命令接口
- 输入: ThrusterCmd(left, right, ttl_ms)
- 输出: ThrusterState(left_rpm, right_rpm, current_a, fault)
- 规则: ttl 到期未刷新则自动降为零推力。

## 13. 安全设计

- 双重急停: 软件急停命令 + 硬件急停输入。
- 指令签名校验: 防止非法注入。
- 关键配置写保护: 仅授权会话可改。
- 最小权限运行: 各进程按需授予设备访问权限。

## 14. 测试与验收

### 14.1 测试分层
- 单元测试: 协议解析、状态机、控制分配、限幅逻辑。
- 集成测试: 端到端命令到推进器闭环。
- 故障注入: 失联、传感器超时、推进器异常、弱网。
- 实船测试: 直航、转向、航点跟踪、应急停机。

### 14.2 验收指标建议
- 控制环频率稳定在目标范围。
- 失联进入 SAFE 时间小于 2.5 s。
- 急停响应时间小于 300 ms。
- 图像回传在弱网下持续可用且不影响控制闭环。

## 15. 建议代码目录

- src/gateway
- src/control
- src/thruster
- src/slam
- src/supervisor
- src/common/message
- src/common/config
- src/common/log
- tests/unit
- tests/integration

## 16. 第一版实施顺序

1. 落地消息协议和进程骨架。
2. 打通 命令 -> 控制 -> 推进器模拟 的最小闭环。
3. 接入真实推进器并完成安全保护。
4. 接入相机和 SLAM 输出，先回传 pose 再回传图像。
5. 完成弱网自适应、故障降级和联调验收。
