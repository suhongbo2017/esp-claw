# COM2 Board — ESP32-S3 with 16MB Flash + Quad PSRAM

| 属性 | 值 |
|------|-----|
| **芯片** | ESP32-S3 (rev v0.2) |
| **Flash** | 16MB (QIO, 80MHz) |
| **PSRAM** | 8MB (Quad, 80MHz) |
| **USB** | USB-OTG (for UVC camera) |
| **UART** | CP2102 (COM20, 115200 baud) |

## 硬件资源

| 外设 | 引脚 | 说明 |
|------|------|------|
| **I2C Master** | SCL=IO40, SDA=IO39 | 通用 I2C 总线 |
| **I2S PDM (麦克风)** | CLK=IO9, DIN=IO17 | 单声道 PDM 麦克风输入 |
| **USB UVC Camera** | USB-DP/DM | 外接 USB 摄像头 |
| **SPI LCD (ST7735S)** | RST=IO15, DC=IO16, MOSI=IO17, SCLK=IO18, CS=IO21 | 1.77" 128x160 彩屏 |
| **DHT (温湿度)** | 任意 GPIO | 通过 I2C 扩展或直连 |

## 固件构建

### 方式一：GitHub Actions（推荐）

1. Fork 本仓库
2. 打开 https://github.com/你的用户名/esp-claw/actions
3. 选择 **"Build COM2 Board"** → **"Run workflow"**
4. 下载生成的 `com2-board-firmware` Artifact

### 方式二：本地构建

```bash
cd application/edge_agent

# 1. 激活 IDF 环境
source /opt/esp/idf/export.sh

# 2. 安装 board manager 工具
pip install esp-bmgr-assist

# 3. 生成板级支持文件
idf.py bmgr -c ./boards -b com2_board

# 4. 解析依赖
idf.py reconfigure

# 5. 编译
idf.py build
```

## 烧录

```bash
cd application/edge_agent/build
esptool --chip esp32s3 -p COM20 -b 460800 write_flash @flash_args
```

## 串口监视

```bash
idf.py -p COM20 monitor
# 或
python -m serial.tools.miniterm COM20 115200
```

## 首次启动

上电后设备会：

1. 初始化 PSRAM（Quad 模式，80MHz）
2. 初始化 I2C 总线
3. 初始化 I2S PDM 麦克风
4. 初始化 USB UVC 摄像头
5. 启动 Wi-Fi 热点（配网模式）
6. 连接热点 `esp-claw-ECC545`，访问 http://192.168.4.1 配置

## 板级配置文件

| 文件 | 说明 |
|------|------|
| `board_info.yaml` | 板子基本信息（芯片、Flash、PSRAM） |
| `board_devices.yaml` | 设备定义（摄像头、音频、I2C） |
| `board_peripherals.yaml` | 外设引脚配置（I2C、I2S） |
| `sdkconfig.defaults.board` | Kconfig 默认配置 |
| `setup_device.c` | 自定义设备初始化代码 |

## 关键配置说明

### OTA 固件升级 (Over-the-Air)

该固件支持通过 HTTP API 进行 OTA 升级，无需物理连接。

**API 端点：**

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/ota/start` | POST | 开始 OTA 升级，JSON body: `{"url": "https://.../edge_agent.bin"}` |
| `/api/ota/status` | GET | 查看 OTA 状态（idle/in_progress） |
| `/api/ota/abort` | POST | 中止正在进行的 OTA 升级 |

**使用示例：**

```bash
# 1. 查看状态
curl http://192.168.4.1/api/ota/status

# 2. 开始升级
curl -X POST http://192.168.4.1/api/ota/start \
  -H "Content-Type: application/json" \
  -d '{"url": "https://example.com/firmware/edge_agent.bin"}'

# 3. 中止升级（可选）
curl -X POST http://192.168.4.1/api/ota/abort
```

**分区表要求：**

OTA 功能需要分区表包含 `ota_0` 和 `ota_1` 分区（已内置在 `partitions_16MB.csv` 中）：

```
# 名称      类型  子类型  偏移量    大小
nvs         data  nvs     0x11000   0x6000
otadata     data  ota     0x10000   0x2000
phy_init    data  phy     0x17000   0x1000
factory     app   factory 0x20000   0x7b0000
ota_0       app   ota_0   -         5M
ota_1       app   ota_1   -         5M
```

### SPI LCD (ST7735S 1.77")

| 参数 | 值 |
|------|-----|
| 驱动芯片 | ST7735S |
| 尺寸 | 1.77 英寸 |
| 分辨率 | 128×160 (竖屏) |
| 接口 | SPI 4线 |
| 色深 | 16bit (RGB565) |
| 时钟频率 | 24 MHz |
| RST | GPIO15 |
| DC | GPIO16 |
| MOSI | GPIO17 |
| SCLK | GPIO18 |
| CS | GPIO21 |

### Quad PSRAM 配置

该板使用 **Quad PSRAM（4线）** 而非常见的 Octal PSRAM（8线），因此需要：

- `CONFIG_SPIRAM_MODE_QUAD=y` — 启用 Quad 模式
- `CONFIG_SPIRAM_XIP_FROM_PSRAM=n` — Quad PSRAM 不支持 XIP
- `CONFIG_SPIRAM_FETCH_INSTRUCTIONS=n` — 指令不从 PSRAM 读取
- `CONFIG_SPIRAM_RODATA=n` — 只读数据不从 PSRAM 读取
- `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=n` — 任务栈使用内部 RAM

### USB UVC 摄像头

- 依赖 `espressif/usb_host_uvc: "2.4.2"`
- 通过 `esp_video_init()` 初始化
- 自动挂载到 `/dev/video40`