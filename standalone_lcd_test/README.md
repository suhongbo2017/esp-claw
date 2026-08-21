# 独立 ST7735S 屏幕测试固件 - 使用说明

## 📦 项目结构

```
standalone_lcd_test/
├── CMakeLists.txt          # ESP-IDF 构建配置
├── sdkconfig.defaults      # ESP-IDF 编译选项
└── main/
    └── main.c              # ST7735S 驱动 + 测试序列
```

**这是一个完全独立的 ESP-IDF 项目**，不包含任何 ESP-Claw 依赖。

## 🔧 硬件信息

| 参数 | 值 |
|------|-----|
| 控制器 | ST7735S |
| 分辨率 | 128 × 160 |
| 接口 | SPI @ 40MHz, RGB565 |
| CS | GPIO21 |
| MOSI | GPIO17 |
| CLK | GPIO18 |
| DC | GPIO16 |
| RST | GPIO15 |

## 🚀 快速部署步骤

### 方法 1：通过 GitHub Actions 构建（推荐）

#### 1. 检查 Actions 是否已运行
打开 https://github.com/suhongbo2017/esp-claw/actions
查看 "Build Standalone LCD Test" workflow 是否正在运行或已完成。

如果没有自动触发，手动点击 **Run workflow** → 选择分支 `lcd-test` → **Run workflow**

#### 2. 等待构建完成（约 5-10 分钟）

#### 3. 下载固件
在 workflow 页面底部找到 **Artifacts**，下载 `lcd-test-binaries.zip`

#### 4. 解压文件
```powershell
mkdir C:\Users\Administrator\lcd_artifacts
unzip -o lcd-test-binaries.zip -d C:\Users\Administrator\lcd_artifacts
```

### 方法 2：本地构建（需要 IDF 环境）

需要安装：
- ESP-IDF v5.5+ 
- MSYS2/Mingw64（Windows）

```bash
cd standalone_lcd_test
idf.py set-target esp32s3
idf.py build
# 输出在 build/*.bin
```

## 💾 烧录到设备

使用 esptool 将固件烧录到 COM19：

```powershell
# 进入固件目录
cd C:\Users\Administrator\lcd_artifacts

# 擦除 flash (可选但推荐)
esptool.py --chip esp32s3 -p COM19 -b 460800 erase_flash

# 创建 flash_args 文件
echo "# Flash configuration for ST7735S test firmware" > flash_args.txt
echo "bootloader.bin                 0x0000" >> flash_args.txt
echo "partitions.bin                 0x8000"   >> flash_args.txt
echo "standalone_lcd_test.bin        0x10000"  >> flash_args.txt

# 烧录
esptool.py --chip esp32s3 -p COM19 -b 460800 write_flash @flash_args.txt

# 验证烧录结果
esptool.py --chip esp32s3 verify_fuses
```

## 🔍 串口监控

烧录完成后立即监控串口输出：

```powershell
python -m serial.tools.miniterm COM19 115200 --raw
```

或者使用 Windows Terminal + Windows Subsystem for Linux：
```bash
sudo screen /dev/ttyUSB0 115200
```

## ✅ 预期行为

成功烧录后，你应该看到：

### 串口日志
```
I (0) CPU: Starting APB...
I (16) cpu_start: Bootloader start
...
I (xxx) LCD_TEST: =========================================
I (xxx) LCD_TEST:   ST7735S LCD TEST - Standalone
I (xxx) LCD_TEST: =========================================
I (xxx) LCD_TEST: Hardware config:
I (xxx) LCD_TEST:   Chip:       ESP32-S3
I (xxx) LCD_TEST:   Screen:     ST7735S 128x160 RGB565
I (xxx) LCD_TEST:   Interface:  SPI @ 40MHz
I (xxx) LCD_TEST:   Pins:
I (xxx) LCD_TEST:     CS=21  MOSI=17  CLK=18
I (xxx) LCD_TEST:     DC=16  RST=15
I (xxx) LCD_TEST: Initializing SPI bus...
I (xxx) LCD_TEST: SPI initialized at 40MHz
I (xxx) LCD_TEST: Initializing ST7735S...
I (xxx) LCD_TEST: ✓ ST7735S initialized!
I (xxx) LCD_TEST: 
=== DISPLAY TEST ===
I (xxx) LCD_TEST: [RED]
I (xxx) LCD_TEST: [GREEN]
I (xxx) LCD_TEST: [BLUE]
I (xxx) LCD_TEST: [Color Bars]
I (xxx) LCD_TEST: [YELLOW]
I (xxx) LCD_TEST: [CYAN]
I (xxx) LCD_TEST: [MAGENTA]
I (xxx) LCD_TEST: [WHITE]
I (xxx) LCD_TEST: [BLACK]
I (xxx) LCD_TEST: 
=== STARTING FLASH LOOP ===
```

### 屏幕显示
1. 红色全屏 → 绿色全屏 → 蓝色全屏（各 1 秒）
2. 8 色彩色条纹（红绿蓝黄青品白黑）
3. 更多纯色展示
4. 最后进入**彩色闪烁循环**（每 400ms 切换一次颜色）

## 🐛 故障排查

### 问题 1: 屏幕完全不亮
- ❌ 检查背光灯供电（GPIO15 同时作为 RST 和 BL）
- ❌ 确认 USB 线支持数据传输（非仅充电线）
- ❌ 检查 SPI 电平是否匹配（ESP32-S3 是 3.3V 逻辑）

### 问题 2: 显示花屏/乱码
- ❌ 可能是引脚定义错误，仔细核对 schematic
- ❌ 尝试降低 SPI 频率：修改 `main.c` 中的 `LCD_SPI_CLK_HZ` 为 10000000（10MHz）
- ❌ 确认 ST7735S 型号而非 ST7789

### 问题 3: 无法连接到串口
- ❌ 检查 COM19 是否被其他程序占用
- ❌ 尝试其他 COM 端口
- ❌ 重启设备后尽快连接（复位时发送数据）

### 问题 4: 烧录失败
- ❌ 按住 BOOT 按钮的同时按下 RESET，然后释放 BOOT
- ❌ 确认 esptool.py 版本 ≥ 4.0
- ❌ 降低波特率: `-b 115200`

### 问题 5: Build 失败 ("MSys/Mingw is no longer supported")
这个问题在使用非官方 ESP-IDF 工具链时可能出现。解决方法：
- 使用 GitHub Actions 构建（推荐，已在 CI 中验证通过）
- 或者安装官方 Espressif IoT Development Framework

## 📝 代码说明

### 关键函数

```c
void app_main(void) {
    // 1. 初始化 GPIO 控制引脚
    gpio_config(&io_conf);
    
    // 2. 配置 SPI 总线
    spi_bus_initialize(SPI2_HOST, &bus_cfg, ...);
    
    // 3. 硬件复位屏幕
    lcd_reset();
    
    // 4. 软件初始化 ST7735S 控制器
    lcd_init();
    
    // 5. 执行测试序列
    show_solid(COLOR_RED, "RED");
    show_solid(COLOR_GREEN, "GREEN");
    // ...
}
```

### 为什么用独立项目？

1. **解耦**: 不依赖 ESP-Claw 的复杂基础设施
2. **快速迭代**: 修改代码后立即测试
3. **清晰边界**: 如果屏幕驱动有问题，可以单独定位
4. **调试友好**: 更少的代码量意味着更容易理解

---
*创建于 2024-08-21 | 目标设备: ESP32-S3 DevKitC-1*
