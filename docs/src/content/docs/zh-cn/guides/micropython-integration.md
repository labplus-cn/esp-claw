---
title: MicroPython 集成指南
description: ESP-Claw 项目中 MicroPython v1.29.0 的构建系统集成、LVGL 绑定与常见问题排查
---

# MicroPython 集成指南

本文档描述 ESP-Claw 项目中 MicroPython 的构建系统集成方案，包括 cmake 基础设施复用、LVGL 绑定的 configure-time 生成、以及常见构建问题的排查方法。

## 构建系统架构

### 整体结构

MicroPython 以 ESP-IDF 组件形式集成在 `components/claw_capabilities/cap_mpy/` 中，复用官方 cmake 基础设施：

```
cap_mpy/
├── CMakeLists.txt              # 主构建文件
├── src/
│   ├── mpconfigport.h          # 端口配置（功能开关）
│   ├── mphalport.h             # HAL 适配层
│   ├── mphalport.c             # HAL 实现
│   ├── cap_mpy.c               # 组件入口
│   ├── cap_mpy_async.c         # 异步任务管理
│   ├── cmd_cap_mpy.c           # CLI 命令
│   └── modmachine_esp32.c      # machine 模块 stub
├── tools/
│   ├── lv_bindings.cmake       # LVGL 绑定生成（configure-time）
│   ├── filter_lv_preprocessed.py  # 预处理过滤器
│   └── extract_lvgl_qstrs.py   # QSTR 提取脚本
└── include/
```

### CMakeLists.txt 关键流程

构建文件按以下顺序组织：

1. **MicroPython 路径设置** — 定位 `py/`、`ports/esp32/`、`extmod/` 等目录
2. **include `py/py.cmake`** — 获取官方源文件列表（`MICROPY_SOURCE_PY` 等）
3. **创建 `usermod` 目标** — `add_library(usermod INTERFACE)`
4. **源文件列表** — 追加 shared/libc/libm/extmod/port 源文件
5. **构建输出目录** — `build/genhdr/`、`build/cap_mpy/`
6. **占位文件** — `file(TOUCH pins.c)` 确保 configure 阶段文件存在
7. **`idf_component_register()`** — 注册为 ESP-IDF 组件
8. **`target_compile_definitions()`** — 编译宏定义（含 `FFCONF_H`）
9. **`target_compile_options()`** — 编译选项（`-Wno-*`）
10. **`file(COPY ffconf.h)`** — 复制 FATFS 配置到 genhdr
11. **`micropy_gather_target_properties()`** — 收集 target 属性
12. **LVGL 绑定** — `include(lv_bindings.cmake)` 生成 `lv_mp.c`
13. **Pin 生成** — 变量定义加入 `MICROPY_SOURCE_QSTR`
14. **include `py/mkrules.cmake`** — QSTR 扫描 + 生成规则
15. **Pin 生成 custom command** — 生成 `pins.h` / `pins.c`

### 关键设计决策

**compile_definitions 必须在 mkrules.cmake 之前**

`mkrules.cmake` 通过 `get_target_property(COMPILE_DEFINITIONS)` 捕获编译宏，用于 QSTR 预处理。如果在 include mkrules 之后才设置 definitions，QSTR 扫描将缺少必要的宏定义。

```cmake
# ✅ 正确顺序
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${DEFINITIONS})
target_compile_options(${COMPONENT_LIB} PRIVATE ${OPTIONS})
micropy_gather_target_properties(${COMPONENT_TARGET})
include(${MICROPY_DIR}/mkrules.cmake)

# ❌ 错误顺序 — QSTR 预处理缺少宏
include(${MICROPY_DIR}/mkrules.cmake)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ${DEFINITIONS})  # 太晚了
```

**FFCONF_H 使用文件名而非完整路径**

QSTR 预处理通过 shell 脚本执行，涉及多层转义。使用文件名 `"ffconf.h"` 配合 `file(COPY)` 将文件复制到 genhdr 目录，让预处理器通过 include 路径找到它。

## LVGL 绑定生成

### configure-time vs build-time

LVGL 绑定（`lv_mp.c`）在 CMake configure 阶段通过 `execute_process()` 生成，而非 build 阶段的 `add_custom_command()`。这确保 fullclean 后文件在 CMake 配置时就已经存在，避免 "Cannot find source file" 错误。

生成流程：

```
lvgl.h → C 预处理器 → 过滤内部头文件 → gen_mpy.py → lv_mp.c
                                                         ↓
                                              extract_lvgl_qstrs.py
                                                         ↓
                                                  qstrdefsport.h
```

### 增量检查

仅在 `lvgl.h` 比 `lv_mp.c` 新时重新生成：

```cmake
file(TIMESTAMP ${LVGL_SRC_DIR}/lvgl.h _lv_ts)
file(TIMESTAMP ${LV_MP_OUTPUT} _out_ts)
if(_out_ts STREQUAL "" OR _lv_ts STRGREATER _out_ts)
    set(_need_lv_gen TRUE)
endif()
```

### 过滤步骤

使用 Python 脚本 `filter_lv_preprocessed.py` 替代 awk，因为 `execute_process` 直接传参给进程，不经过 shell，awk 的复杂转义会失败。

过滤规则：排除 `lv_obj_style_internal.h` 和 `lv_obj_style_internal_gen.h` 的内容，避免内部类型定义干扰绑定生成。

### QSTR 循环依赖解决

LVGL 绑定存在经典的 QSTR 循环依赖：
- `lv_mp.c` 包含大量 `MP_QSTR_xxx` 引用，编译时需要 `qstrdefs.generated.h`
- `qstrdefs.generated.h` 的生成需要扫描所有源文件（包括 `lv_mp.c`）

解决方案：先生成 `lv_mp.c`，然后从中提取所有 `MP_QSTR_xxx` 引用写入 `qstrdefsport.h`，打破循环。

## execute_process 与 add_custom_command 差异

| 特性 | `execute_process` | `add_custom_command` |
|------|-------------------|---------------------|
| 执行时机 | CMake configure 阶段 | Build 阶段 |
| 参数传递 | 直接传给进程，不经 shell | 经 Ninja/Make + shell 解析 |
| 转义规则 | CMake list 转义 | Shell 转义 + Ninja 转义 |
| 增量构建 | 需手动检查时间戳 | 自动基于 OUTPUT/DEPENDS |
| 适用场景 | 生成 configure 阶段需要的文件 | 生成 build 阶段的编译产物 |

**注意**：从 `add_custom_command` 迁移到 `execute_process` 时，awk 等需要复杂 shell 转义的工具应替换为 Python 脚本。

## 常见问题排查

### fullclean 后 "Cannot find source file"

**现象**：`rm -rf build && idf.py build` 时报 `Cannot find source file: build/lv_mp.c`

**原因**：`lv_mp.c` 是生成文件，fullclean 后不存在，CMake configure 阶段验证失败。

**解决**：在 `idf_component_register()` 之前创建占位文件：

```cmake
file(TOUCH ${CMAKE_BINARY_DIR}/pins.c)  # build-time 生成需要占位
# lv_mp.c 已在 configure-time 由 lv_bindings.cmake 生成，无需占位
```

### Python 解释器不一致

**现象**：`idf.py flash` 提示 `'python' is currently active while the project was configured with 'python3'`

**原因**：构建时用 `python3 idf.py build`，烧录用 `idf.py flash`（默认 `python`），CMake 缓存记录了不同的解释器路径。

**解决**：始终使用 `idf.py build`（不加 `python3` 前缀），确保与 `idf.py flash` 使用同一解释器。

### I2C 驱动冲突

**现象**：启动时 `CONFLICT! driver_ng is not allowed to be used with this old driver` 然后 abort

**原因**：MicroPython 的 `machine_i2c.c` 使用旧版 I2C API，但项目其他组件（板管理器、Lua 驱动等）使用新版 `driver/i2c_master.h`，两者同时链接触发冲突检查。

**解决**：暂时禁用 MicroPython I2C 驱动：

```c
// mpconfigport.h
#define MICROPY_PY_MACHINE_I2C       (0)
#define MICROPY_PY_MACHINE_SOFTI2C   (0)
```

```cmake
# CMakeLists.txt — 移除 machine_i2c.c 源文件
```

后续需要将 `machine_i2c.c` 迁移到新版 I2C API。

### sys_evt 任务栈溢出

**现象**：`Guru Meditation Error: Stack protection fault in task "sys_evt"`

**原因**：默认 `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304` 不足以处理大量事件日志。

**解决**：在 `sdkconfig.defaults` 中增加栈大小：

```
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096
```

### LVGL 显示缓冲区分配失败

**现象**：`alloc primary buffer 38400 bytes failed`

**原因**：MIPI DSI 配置的 LVGL profile 默认 `use_psram = false`，缓冲区从内部内存分配，可能因内存不足失败。

**解决**：在 `display_service.c` 中覆盖为使用 PSRAM：

```c
#ifdef CONFIG_SPIRAM
disp_cfg.profile.use_psram = true;
#endif
```

### managed_components 哈希不匹配

**现象**：`idf.py fullclean` 报 `Hash of the file does not match expected hash`

**原因**：`managed_components` 中的文件被本地修改，与组件管理器记录的哈希不一致。

**解决**：使用 `rm -rf build` 代替 `idf.py fullclean`，避免触发 managed_components 检查。

## 构建命令参考

```bash
# 导出 ESP-IDF 环境
export IDF_PATH="/home/jiang/.espressif/v5.5.4/esp-idf"
source "$IDF_PATH/export.sh"

# 生成 board manager 配置
cd application/edge_agent
idf.py bmgr -c ./boards -b <board_name>

# 完整 clean build
rm -rf build
idf.py build

# 烧录
idf.py flash monitor
```

## 功能开关参考

`mpconfigport.h` 中的关键配置：

| 宏 | 当前值 | 说明 |
|----|--------|------|
| `MICROPY_PY_MACHINE` | 1 | machine 模块 |
| `MICROPY_PY_MACHINE_PIN_MAKE_NEW` | `mp_pin_make_new` | Pin 类构造 |
| `MICROPY_PY_MACHINE_I2C` | 0 | 硬件 I2C（待迁移新版 API） |
| `MICROPY_PY_MACHINE_SOFTI2C` | 0 | 软件 I2C |
| `MICROPY_PY_MACHINE_SPI` | 0 | 硬件 SPI |
| `MICROPY_PY_MACHINE_PWM` | 0 | PWM |
| `MICROPY_PY_MACHINE_ADC` | 0 | ADC |
| `MICROPY_PY_MACHINE_UART` | 0 | UART |
| `MICROPY_ENABLE_SCHEDULER` | 1 | 调度器支持 |
| `MICROPY_PY_MACHINE_RESET` | 1 | machine.reset() |
| `MICROPY_REPL_EVENT_DRIVEN` | 1 | 事件驱动 REPL |
| `MICROPY_KBD_EXCEPTION` | 1 | Ctrl+C 支持 |
