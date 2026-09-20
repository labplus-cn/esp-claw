# MMC5603NJ 规格书（转换 MD）

> 器件：±30G 三轴 AMR 磁传感器 MMC5603NJ | Rev.B
>
> 本文件为供应商规格书的 Markdown 转换版，仅供驱动开发参考，最终以官方 PDF 为准。

## FEATURES

- 单片集成三轴 AMR 磁传感器与电路，外围器件少
- 动态范围 ±30G，20bit 模式分辨率 0.0625 mG/LSB，总 RMS 噪声 2 mG，航向精度 ±1°
- 传感器真实频率响应最高 1 kHz
- WLP 晶圆级封装：**0.8×0.8×0.4 mm**
- 片上自动消磁 SET/RESET：消除温度带来零点漂移，清除强磁场剩磁
- 片上灵敏度补偿、内置温度传感器
- 支持自检信号；I3C 可提供 Data_ready 中断
- 休眠电流 1 µA
- I2C Fast 模式 ≤400 kHz；支持 I3C 接口
- 供电：**1.62-3.6 V**；IO 逻辑电平 1.2 V
- RoHS 合规

## APPLICATIONS

电子罗盘、GPS 导航、位置检测

## DESCRIPTION

MMC5603NJ 为完整单片三轴 AMR 磁传感器，内置信号处理与数字 I2C/I3C 接口，可直连 MCU，无需外部 ADC。
量程 ±30 Gauss；20bit 模式 0.0625 mG/LSB，RMS 噪声 2 mG，典型航向精度 ±1°。
SET/RESET 消除零点随温漂，清除强磁铁带来剩磁；可每次测量执行或周期执行。
WLP 0.8×0.8×0.4 mm；工作温度 **-40℃ ~ +85℃**。

## 电气参数（25℃，VDD=1.8V，Auto_SR_en=1）

| Parameter | Conditions | Min | Typ | Max | Units |
|---|---|---|---|---|---|
| Field Range (Each Axis) | Total applied field | | ±30 | | G |
| Supply Voltage | VDD | 1.62 | 1.8 | 3.6 | V |
| VIO | | 1.2 | 1.8 | VDD | V |
| Supply Voltage rise time | | | | 10.0 | ms |
| Supply Current 100Hz | BW=00 | | 3.4 | 4.0 | mA |
| | BW=01 | | 2.4 | 3.0 | mA |
| | BW=10 | | 1.3 | 1.6 | mA |
| | BW=11 | | 0.75 | 1.0 | mA |
| Power Down Current | | | 1.0 | 1.2 | µA |
| Operating Temperature | | -40 | | 85 | ℃ |
| Storage Temperature | | -55 | | 125 | ℃ |
| Linearity Error | FS=±30G, H=±15G | | 0.5 | 0.75 | %FS |
| Hysteresis | 3 sweeps ±30G | | 0.02 | 0.1 | %FS |
| Repeatability Error | 3 sweeps ±30G | | 0.02 | 0.1 | %FS |
| Alignment Error | | | ±1.0 | ±3.0 | Deg |
| Transverse Sensitivity | | | ±2.0 | ±5.0 | % |
| Total RMS Noise | BW=00 | | 1.5 | 2.5 | mG |
| | BW=01 | | 2.0 | 4.0 | mG |
| | BW=10 | | 3.0 | 5.0 | mG |
| | BW=11 | | 4.0 | 7.0 | mG |
| Output resolution | | | 20 | | Bits |
| Max Output data rate | BW=00 | 75 | | | Hz |
| | BW=01 | 150 | | | Hz |
| | BW=10 | 255 | | | Hz |
| | BW=11 | 255 | | 1000 | Hz |
| Heading accuracy | | | ±1.0 | ±3.0 | Deg |
| Sensitivity 20-bit | ±30G | | 16384 | | counts/G |
| Null-field output 20bit | | | 524288 | | Counts |
| Null-field temp drift | | | ±0.2 | ±1.0 | mG/℃ |
| Temp sensor | | 0.6 | 0.8 | 1.0 | ℃/count |
| Disturbing Field | | 32 | | | G |
| Max Exposed Field | | | | 10000 | G |

### I2C IO 特性（VIO=1.8V）

| Parameter | Symbol | Min | Typ | Max | Unit |
|---|---|---|---|---|---|
| Input low | VIL | -0.5 | | 0.3*VIO | V |
| Input high | VIH | 0.7*VIO | | VIO | V |
| Schmitt 滞回 | Vhys | 0.2 | | | V |
| Output low | VOL | | | 0.4 | V |
| Input leakage | Ii | -10 | | 10 | µA |
| SCL freq | fSCL | 0 | | 400 | kHz |

### 绝对最大额定

- VDD：-0.5 ~ +5 V
- 存储温度：-55 ~ +125 ℃
- 最大暴露磁场：10000 G

## WLP 引脚定义

| Pin | Name | Description | I/O |
|---|---|---|---|
| A1 | VSA | GND | Power |
| A2 | SCL | I2C 时钟 | Input |
| B1 | VDD | 电源 | Power |
| B2 | SDA | I2C 数据 | I/O |

> ⚠️ ESD 敏感器件

## 外部电路连接

- VDD 靠近引脚放置 ≥2.2 µF 去耦电容
- VSA 直接接地
- I2C 上拉 Rp：短总线 <10cm 选 **2.7 kΩ**；<5cm 可用 10 kΩ
- VIO 可与 VDD 共域；SDA/SCL 上拉至 VIO 域

### PCB 硬件要点

1. 远离扬声器、线圈、电感等磁源；远离大电流走线，不要在传感器下方布功率走线
2. 屏蔽罩、电池、含铁材料不要贴传感器或 PCB 反面

## 寄存器映射

| Reg Name | Addr | Description |
|---|---|---|
| Xout0 | 0x00 | X[19:12] |
| Xout1 | 0x01 | X[11:4] |
| Yout0 | 0x02 | Y[19:12] |
| Yout1 | 0x03 | Y[11:4] |
| Zout0 | 0x04 | Z[19:12] |
| Zout1 | 0x05 | Z[11:4] |
| Xout2 | 0x06 | X[3:0] |
| Yout2 | 0x07 | Y[3:0] |
| Zout2 | 0x08 | Z[3:0] |
| Tout | 0x09 | 温度输出 |
| Status1 | 0x18 | 状态1 |
| ODR | 0x1A | 输出数据率 |
| Internal control 0 | 0x1B | 控制0 |
| Internal control 1 | 0x1C | 控制1 |
| Internal control 2 | 0x1D | 控制2 |
| ST_X_TH | 0x1E | X 自检阈值 |
| ST_Y_TH | 0x1F | Y 自检阈值 |
| ST_Z_TH | 0x20 | Z 自检阈值 |
| ST_X | 0x27 | X 自检设定 |
| ST_Y | 0x28 | Y 自检设定 |
| ST_Z | 0x29 | Z 自检设定 |
| Product ID | **0x39** | 芯片 ID，只读，值=**0x10** |

### Status1 (0x18) 位定义

| Bit | Name | Description |
|---|---|---|
| 4 | OTP_read_done | OTP 加载完成，上电后必须等待此 bit=1 再读 ID |
| 1 | Meas_m_done | 磁场测量完成，读磁场寄存器自动清零 |

> 注：实测硬件中测量完成标志（MM_DONE）为 Status1 的 Bit6 (0x40)；本规格书摘要标注为 Bit1。以硬件实测为准。

### Control0 (0x1B)

- Bit0 `Take_meas_M`：触发单次磁测量，写 1 自清除
- Bit3 `Do_Set`：执行 SET 脉冲 (375 ns)，写 1 自清除
- Bit4 `Do_Reset`：执行 RESET 脉冲，写 1 自清除
- Bit5 `Auto_SR_en`：**建议置 1**，开启自动 SET-RESET

### Control1 (0x1C) BW 带宽

| BW1 | BW0 | 单次测量时间 |
|---|---|---|
| 0 | 0 | 6.6 ms |
| 0 | 1 | 3.5 ms |
| 1 | 0 | 2.0 ms |
| 1 | 1 | 1.2 ms |

### I2C 从机地址

7bit 地址：**0x30**；写：0x60；读：0x61

### SET-RESET 消除零点算法

1. SET；测量得到 Out1 = +H + Offset
2. RESET；测量得到 Out2 = -H + Offset
3. 磁场：`H = (Out1-Out2)/2`；零点 `Offset = (Out1+Out2)/2`

### 时序要点

- VDD 上电后至少等待 **5 ms**；建议等待 `OTP_read_done==1`
- SET/RESET 后至少间隔 **1 ms** 再做其他 I2C 操作

### 焊接回流

峰值温度 **260℃，最长 10 s**；MSL-1 等级。
