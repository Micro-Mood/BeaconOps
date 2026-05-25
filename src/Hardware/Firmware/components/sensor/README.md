# Sensor

这是 BeaconOps 里**LSM6DS3TR-C** 六轴 IMU 的封装层，只管三件事：初始化、读原始数据、配芯片自带的中断能力。

它**不**决定“什么叫摇晃”“什么叫撑起来”这种业务逻辑——那些都是 `sensor_task` 和 `main/main.cpp` 在干。反过来，你想把这个驱动拿去别的项目用，也不会被 BeaconOps 的业务逻辑绮住。

## 文档导航

- 上层 components 总览：`../README.md`
- 英文版：`README_EN.md`

---

## 依赖

- `I2C/i2c_bus_manager.h`：共享 I2C 总线访问
- `lsm6ds3trc.h`：芯片寄存器级驱动
- `config.h`：I2C 引脚和频率配置由全局配置统一管理

---

## 提供的能力

### 基础能力

- `lsm6ds3tr_c_esp32_init`：初始化设备并绑定 I2C 总线
- `lsm6ds3tr_c_check_connection`：检查芯片是否在线
- `lsm6ds3tr_c_read_data`：读取加速度、陀螺仪和温度等原始数据
- `lsm6ds3tr_c_get_ctx`：获取底层寄存器访问上下文

### 采样参数配置

- `lsm6ds3tr_c_config_accel`：配置加速度采样率与量程
- `lsm6ds3tr_c_config_gyro`：配置陀螺仪采样率与量程

### 中断与事件能力

- `lsm6ds3tr_c_config_interrupts`：配置中断引脚和 GPIO
- `lsm6ds3tr_c_register_int_callback`：注册中断回调
- `lsm6ds3tr_c_enable_interrupt`：使能或关闭指定中断源
- `lsm6ds3tr_c_read_int_status`：读取当前中断状态

### 芯片内建检测功能

- `lsm6ds3tr_c_config_tap_detection`：单击 / 双击检测
- `lsm6ds3tr_c_config_free_fall_detection`：自由落体检测
- `lsm6ds3tr_c_config_step_detection`：步数检测
- `lsm6ds3tr_c_config_wake_up_detection`：唤醒检测
- `lsm6ds3tr_c_config_inactivity_detection`：静止 / 不活动检测
- `lsm6ds3tr_c_config_6d_orientation`：6D 姿态检测

---

## 在 BeaconOps 中的角色

在本项目里，`sensor/` 只负责提供原始 IMU 数据和底层中断配置：

1. `app_sensor_init()` 创建传感器句柄并完成基础初始化。
2. `sensor_task` 在运行时持续采样。
3. `sensor_task` 将原始数据转换为“运动 / 摇晃 / 行为”回调，再转发给 `msg` 与 `profile`。

因此如果你想修改：

- **I2C 接线、量程、采样率**：看本组件和 `config.h`
- **摇晃判定、行为节流、业务联动**：看 `sensor_task` 和 `main/main.cpp`

---

## 注意事项

- 本组件是硬件驱动层，不负责 UI、消息上报或提示音。
- 真实业务语义由上层任务定义，避免把项目特有逻辑直接写进驱动层。
- 如果更换 IMU 型号，优先保持本组件对外接口不变，这样上层装配代码改动最小。