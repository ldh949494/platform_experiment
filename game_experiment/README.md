# Game Experiment

这是一个基于 PlatformIO 框架的嵌入式开发实验项目。

## 目录结构说明

- `src/`: 源代码存放目录，包含主要的应用逻辑。
- `include/`: 头文件存放目录。
- `lib/`: 本地库文件存放目录。可将自定义的模块或外部库放在此处。
- `test/`: 单元测试代码目录。
- `platformio.ini`: PlatformIO 项目配置文件，定义了开发板型号、编译选项、依赖库等。

## 环境依赖

* [PlatformIO](https://platformio.org/): 推荐在 VSCode 中安装 PlatformIO IDE 插件以获得最佳开发体验。

## 编译与运行

使用 PlatformIO 提供的工具或 IDE 插件进行编译和烧录：

1. 编译项目：
   ```bash
   pio run
   ```

2. 烧录到开发板：
   ```bash
   pio run --target upload
   ```

3. 清理编译产物：
   ```bash
   pio run --target clean
   ```

## 注意事项

- 本地编译产物（如 `.pio/` 目录下的文件、`*.elf`、`*.bin` 等）和 IDE 配置文件（如 `.vscode/` 目录）已在 `.gitignore` 中配置忽略，不会提交到版本控制系统中。
