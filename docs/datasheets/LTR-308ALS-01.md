# LTR-308ALS-01 光传感器规格书（转换 MD）

> Lite-On 环境光传感器 LTR-308ALS-01
>
> 本文件为供应商规格书的 Markdown 转换版，仅供驱动开发参考，最终以官方 PDF 为准。

## Description

I2C 数字环境光传感器，2×2 mm chipled 封装；动态范围 0.01~157 klux；接近人眼光谱响应；50/60Hz 频闪抑制；支持中断；休眠功耗极低。

## Features

- I2C：标准 100 kHz / Fast 400 kHz
- VDD：**1.7-3.6 V**；工作温度 -40 ~ +85 ℃
- 分辨率：16-20bit
- 中断带滞回，降低 MCU 轮询开销
- RoHS、无卤素

## Applications

显示屏亮度自动调节，移动设备、消费电子

## Pinout（6-pin chipled）

| Pin | Symbol | Description |
|---|---|---|
| 1 | VDD | 电源 |
| 2 | NC | 悬空 |
| 3 | GND | 地 |
| 4 | SCL | I2C 时钟 (开漏输入) |
| 5 | INT | 中断输出，开漏，低有效 |
| 6 | SDA | I2C 数据 (开漏 I/O) |

### 外围电路

- VDD：0.1 µF + 4.7 µF 去耦
- SDA/SCL 上拉电阻：1-10 kΩ 到 Vbus
- INT 外部上拉；噪声环境信号线对地加 10 pF 滤波

## I2C 从机地址

7-bit：**0x53**

- 写地址：`0xA6`
- 读地址：`0xA7`

> 注：LTR-308ALS 的 I2C 地址会因 ADDR 引脚硬件连接不同而变化（常见 0x29 / 0x53），实测确认须以 `test i2c` 扫描结果为准。

## 寄存器简表

| Addr | R/W | Register | Reset | Description |
|---|---|---|---|---|
| 0x00 | RW | MAIN_CTRL | 0x00 | 模式、软件复位 |
| 0x04 | RW | ALS_MEAS_RATE | 0x22 | 分辨率、测量速率 |
| 0x05 | RW | ALS_GAIN | 0x01 | 模拟增益 |
| **0x06** | R | **PART_ID** | **0xB1** | 器件 ID |
| 0x07 | R | MAIN_STATUS | 0x20 | 上电状态、中断、数据就绪 |
| 0x0D-0x0F | R | ALS_DATA0-2 | 0x00 | ALS ADC 20bit 输出 |
| 0x19 | RW | INT_CFG | 0x10 | 中断配置 |
| 0x1A | RW | INT_PST | 0x00 | 中断滤波 persist |
| 0x21-0x23 | RW | ALS_THRES_UP | FF FF 0F | 中断上限阈值 |
| 0x24-0x26 | RW | ALS_THRES_LOW | 00 00 00 | 中断下限阈值 |

### MAIN_CTRL (0x00)

- bit1：`ALS_Enable`：1=激活；0=待机
- bit4：`SW_Reset`：写 1 软件复位

### ALS_MEAS_RATE (0x04)

分辨率（bit6-4）

| Value | Bit | 积分时间 |
|---|---|---|
| 000 | 20bit | 400 ms |
| 001 | 19bit | 200 ms |
| 010 | 18bit (default) | 100 ms |
| 011 | 17bit | 50 ms |
| 100 | 16bit | 25 ms |

测量速率 bit2-0：25/50/100/500/1000/2000 ms

### ALS_GAIN (0x05) 增益

| Gain code | GAIN |
|---|---|
| 000 | ×1 |
| 001 | ×3 (default) |
| 010 | ×6 |
| 011 | ×9 |
| 100 | ×18 |

### MAIN_STATUS (0x07)

- bit5 Power-On status：上电标记，读后清零
- bit4 ALS Interrupt status：中断标记
- bit3 ALS Data status：新数据标记

### Lux 计算公式

```
Lux = (0.6 × ALS_ADC) / (GAIN × INT) × WindowFactor
```

> INT：20bit=4；19bit=2；18bit=1；17bit=0.5；16bit=0.25

### 工作状态机

1. Standby 待机（默认上电）
2. 写 ALS_Enable=1 唤醒，等待振荡器稳定 typ 5 ms
3. ALS 转换循环；写 0 回到待机，当前 ADC 转换完成后休眠

### 中断行为

INT 引脚开漏低有效；`INT_CFG bit2=1` 开启中断；连续 N 次超出阈值触发；读 `MAIN_STATUS` 清除中断标志与 INT 引脚电平。

### 回流焊接

峰值 260 ℃，高于 217 ℃ 保持 60-90 s；最多两次回流。

### 防潮等级

JEDEC J-STD-033A Level 3；拆封后 7 天内焊接；超时需烘烤：60 ℃/48h (卷带) 或 100 ℃/4h (散装)。
