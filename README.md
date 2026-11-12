# 基于 Whisper.cpp 与 C++ 的边缘端实时音频监听与转写系统

## 1. 简介

WAVtoTXT 是一个基于 C++ 开发的自动语音转文字程序，使用 "whisper.cpp"、"WtoTClient.cpp"、"ggml-base.bin" 实现中文语音识别。

模型下载地址：```https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin```

WtoTClient.cpp通过监听指定目录，自动检测新生成的 WAV 音频文件，并将其加入识别任务队列。系统按照音频文件的时间顺序依次处理任务，完成音频读取、格式转换、语音识别和文本保存，实现 WAV 音频到 TXT 文本的自动转换。

本工程适用于持续产生音频文件的语音采集场景，例如车载语音采集、设备语音记录和连续音频监测等。

### 主要功能

- **自动文件监听**：使用 Linux `inotify` 机制监听 WAV 文件目录。
- **自动任务调度**：检测新生成的 WAV 文件，并自动加入识别队列。
- **时间顺序处理**：根据文件名中的时间戳对任务排序。
- **文件完整性检查**：检测文件大小是否稳定，避免读取尚未写入完成的音频。
- **重复任务过滤**：通过任务集合和处理记录避免重复识别。
- **音频预处理**：支持多声道转单声道以及音频重采样。
- **中文语音识别**：调用 Whisper.cpp 执行中文语音识别。
- **文本自动保存**：将识别结果写入 TXT 文件。
- **性能统计**：记录 Whisper 推理耗时，便于评估系统运行效率。

---

## 2. 系统架构

系统采用模块化处理流程，主要包括文件监听模块、任务调度模块、音频预处理模块、语音识别模块和文本保存模块。

                  WAV 音频采集设备
                         |
                         v
                  WAV 文件目录
                       Wav/
                         |
                         v
                 +---------------+
                 | 文件监听模块  |
                 |    inotify    |
                 +---------------+
                         |
                         v
                 +---------------+
                 | 文件完整性检查|
                 | 文件大小稳定性|
                 +---------------+
                         |
                         v
                 +---------------+
                 | 任务调度模块  |
                 | 时间戳排序    |
                 | 重复任务过滤  |
                 +---------------+
                         |
                         v
                 +---------------+
                 | 音频预处理模块|
                 | WAV 文件读取  |
                 | 多声道转单声道|
                 | 重采样至16kHz |
                 +---------------+
                         |
                         v
                 +---------------+
                 | Whisper.cpp   |
                 | 中文语音识别  |
                 +---------------+
                         |
                         v
                 +---------------+
                 | 文本处理模块  |
                 | 文本提取      |
                 | 首尾空白清理  |
                 +---------------+
                         |
                         v
                  TXT/result.txt


### 2.1 文件监听模块

程序使用 Linux `inotify` 监听 `Wav/` 目录。

主要监听以下事件：

- `IN_CLOSE_WRITE`：文件写入完成并关闭。
- `IN_MOVED_TO`：文件通过移动或重命名进入监听目录。

当检测到新的 WAV 文件后，程序将其加入待处理任务队列。

### 2.2 文件完整性检查

为了避免在音频文件尚未写入完成时启动识别，程序会检查文件大小变化。

当前实现采用以下判断逻辑：

1. 获取 WAV 文件当前大小。
2. 间隔约 1 秒再次获取文件大小。
3. 判断两次文件大小是否一致。
4. 文件大小大于 WAV 文件头的基本长度（44 字节），且连续两次检测大小不变时，认为文件已稳定。

该机制用于降低读取不完整音频文件。

### 2.3 任务调度模块

程序使用优先队列管理待识别文件，并按照文件名中的时间戳排序。

文件命名格式：YYYY_MM_DD_HH_MM_SS.wav

例如：

2026_11_12_11_35_27.wav
2026_11_12_11_35_30.wav
2026_11_12_11_35_33.wav

程序启动时会扫描已有 WAV 文件，并将符合条件的文件加入任务队列。

对于新检测到的文件，程序通过 scheduledFiles 等任务管理机制进行去重，避免同一文件被重复加入队列。

### 2.4 音频预处理模块

程序使用以下两个第三方库处理 WAV 音频：

- **libsndfile**：负责 WAV 音频文件读取。
- **libsamplerate**：负责音频重采样。

预处理主要包括：

1. 读取 WAV 音频数据。
2. 将多声道音频转换为单声道。
3. 将音频重采样至 16 kHz。
4. 将处理后的音频数据转换为 `Whisper` 所需的浮点型采样数据。

Whisper 模型使用 16 kHz 单声道音频作为输入。

### 2.5 语音识别模块

项目采用 `whisper.cpp` 作为语音识别引擎。

模型文件：

```WhisperModule/ggml-base.bin```

当前使用 Whisper Base 模型，识别语言设置为中文。

核心识别函数：

whisper_full(
    ctx,
    params,
    samples.data(),
    static_cast<int>(samples.size())
);

主要参数设置包括：

| 参数 | 作用 |
|---|---|
| `WHISPER_THREADS` | 设置 Whisper 内部计算线程数 |
| `params.language` | 设置识别语言为中文 `zh` |
| `params.translate` | 设置是否翻译，当前关闭 |
| `params.no_context` | 关闭跨音频片段上下文复用 |
| `params.print_progress` | 关闭识别进度输出 |
| `params.print_realtime` | 关闭实时文本输出 |
| `params.print_timestamps` | 关闭时间戳输出 |

当前采用贪心采样策略：

whisper_full_default_params(
    WHISPER_SAMPLING_GREEDY
);

### 2.6 文本保存模块

识别完成后，程序通过 Whisper 提供的接口获取识别片段：

``` whisper_full_n_segments(ctx); ```

``` whisper_full_get_segment_text(ctx, i);```

程序遍历识别结果，将各个片段拼接成完整文本，并清理文本首尾的空白字符。

识别结果保存至：

``` Txt/result.txt ```

具体文件保存方式以 `src/WtoTClient.cpp` 中的实现为准。

## 3. 项目目录结构
```
WavtoTxt/
│
├── CMakeLists.txt
│
├── src/
│   └── WtoTClient.cpp
│
├── whisper.cpp/
│   └── Whisper.cpp 源码及相关文件
│
├── WhisperModule/
│   └── ggml-base.bin
│
├── Wav/
│   └── WAV 音频文件存放目录
│
├── Txt/
│   └── result.txt
│
├── build/
│   └── x86_64 平台编译目录
│
└── build_arm64/
    └── ARM64 平台编译目录
```
说明：

- `src/`：项目主程序源代码。
- `whisper.cpp/`：Whisper.cpp 及其推理依赖源码。
- `WhisperModule/`：语音识别模型文件。
- `Wav/`：待识别 WAV 文件目录。
- `Txt/`：识别文本输出目录。
- `build/`：Ubuntu x86_64 平台构建目录。
- `build_arm64/`：ARM64 平台构建目录。

`build/` 和 `build_arm64/` 均为编译生成目录，不建议直接在不同架构之间复制使用。

## 4. 运行环境

### 4.1 软件环境

| 项目 | 配置 |
|---|---|
| 操作系统 | Ubuntu 22.04 LTS |
| 编程语言 | C++17 |
| 构建工具 | CMake |
| 编译器 | GCC / G++ |
| 语音识别引擎 | whisper.cpp |
| 音频读取库 | libsndfile |
| 音频重采样库 | libsamplerate |
| 线程支持 | POSIX Threads |

### 4.2 硬件平台

目前项目支持在以下架构环境中编译运行：

- x86_64：Ubuntu 虚拟机或 Linux PC。
- ARM64：基于 RK3588 的 Linux 开发板。

不同平台需要分别进行本机编译。


## 5. 环境依赖安装

在 Ubuntu 22.04 环境下执行：

``` sudo apt update ```

``` sudo apt install -y \
    build-essential \
    cmake \
    pkg-config \
    git \
    libsndfile1-dev \
    libsamplerate0-dev
```

检查编译环境：

```bash
gcc --version
g++ --version
cmake --version
pkg-config --modversion sndfile
pkg-config --modversion samplerate
```

## 6. 项目编译

### 6.1 获取项目

```bash
git clone <YOUR_REPOSITORY_URL>
cd WavtoTxt
```

如果 `whisper.cpp/` 已经作为完整源码目录提交到仓库，则无需重复初始化。

### 6.2 x86_64 平台编译

在 x86_64 Ubuntu 环境中执行：

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DGGML_NATIVE=ON
```

编译：

```bash
cmake --build build -j4
```

编译成功后，生成：

``` build/wavtotxt ```

### 6.3 ARM64 平台编译

在 ARM64 开发板本机执行：

```bash
cmake -S . -B build_arm64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DGGML_NATIVE=ON
```

编译：

```bash
cmake --build build_arm64 -j4
```

编译成功后，生成：

``` build_arm64/wavtotxt ```

注意：

`GGML_NATIVE=ON` 用于根据当前编译设备的 CPU 特性进行本机优化。不同 CPU 架构的构建目录不应混用。

---

## 7. 模型文件配置

项目当前使用 Whisper Base 模型。

模型文件：

```WhisperModule/ggml-base.bin```

运行前需要确认模型文件存在：

```bash
ls -lh WhisperModule/ggml-base.bin
```

如果模型文件没有包含在 Git 仓库中，需要按照项目约定单独下载并放置到指定目录。

注意：模型文件必须与 Whisper.cpp 支持的模型格式相匹配。

---

## 8. 程序运行

运行前确认以下目录和文件已经准备好：

```text
Wav/
Txt/
WhisperModule/ggml-base.bin
```

如果输出目录不存在，需要提前创建：

```bash
mkdir -p Wav Txt
```
或者修改WtoTClient.cpp程序以及CMakeLists.txt中指定目录：
```
const std::string ROOT_DIR = "/home/cat/WavtoTxt/";
// directories
const std::string INPUT_DIR = ROOT_DIR + "Wav";

const std::string OUTPUT_DIR = ROOT_DIR + "Txt";

const std::string RESULT_FILE = ROOT_DIR + 
    "Txt/result.txt";

const std::string RECORD_FILE = ROOT_DIR + 
    "Txt/processed.txt";

const std::string MODEL_FILE = ROOT_DIR + 
    "WhisperModule/ggml-base.bin";
```
``` add_executable(wavtotxt /home/cat/WavtoTxt/src/WtoTClient.cpp)```

启动程序：

```bash
./build/wavtotxt
```

ARM64 平台：

```bash
./build_arm64/wavtotxt
```

注意：上述命令假设源码中的模型路径和工作目录配置与当前项目目录一致。若源码使用绝对路径，应先根据实际部署目录修改相关路径常量。

程序启动后会持续监听 WAV 文件目录。

当新文件生成时，终端会显示类似信息：

```text
[监听] 发现新文件: 2026_11_12_11_35_27.wav

[识别] 正在处理:
/home/cat/WavtoTxt/Wav/2026_11_12_11_35_27.wav

[性能] Whisper 推理耗时：1.80 秒

[COMPLETED] 2026_11_12_11_35_27.wav -> 识别文本
```

按下 `Ctrl+C` 可以终止程序。

---

## 9. 音频文件要求

当前程序面向 WAV 音频输入。

建议使用以下音频配置：

| 参数 | 推荐配置 |
|---|---|
| 文件格式 | WAV |
| 声道 | 单声道或多声道 |
| 目标采样率 | 16000 Hz |
| 模型输入 | 单声道浮点型音频 |
| 语言 | 中文 |
| 文件命名 | YYYY_MM_DD_HH_MM_SS.wav |

程序内部会执行声道转换和重采样，因此输入音频不一定必须是单声道 16 kHz，但必须能够被 libsndfile 正常读取。

文件名建议严格采用时间戳格式，以确保任务能够按照预期顺序处理。

---

## 10. 任务去重与处理记录

为了避免重复识别，程序采用任务状态管理机制。

主要包括：

- `scheduledFiles`：记录已经加入调度流程的文件，避免重复入队。
- `processed.txt`：保存已经处理过的文件记录，支持程序重新启动后的任务去重。

程序启动时会结合目录扫描和处理记录判断哪些文件需要重新加入识别队列。

具体任务恢复及异常处理行为以当前源码实现为准。

---

## 11. 性能测试与优化

### 11.1 推理耗时统计

程序通过 C++ `std::chrono` 统计 Whisper 推理耗时。

计时位置位于 `whisper_full()` 调用前后：

```cpp
auto infer_start =
    std::chrono::steady_clock::now();

int ret = whisper_full(
    ctx,
    params,
    samples.data(),
    static_cast<int>(samples.size())
);

auto infer_end =
    std::chrono::steady_clock::now();

double infer_time =
    std::chrono::duration<double>(
        infer_end - infer_start
    ).count();

std::cout << "[性能] Whisper 推理耗时："
          << infer_time << " 秒"
          << std::endl;
```

该计时主要反映 Whisper 模型推理耗时，不包含完整的文件监听、音频读取、重采样及文本保存流程。

### 11.2 线程数配置

线程数通过源码中的常量控制：

```cpp
const int WHISPER_THREADS = 8;
```

在识别参数中设置：

```cpp
params.n_threads = WHISPER_THREADS;
```

可以根据实际运行平台调整线程数。

不同线程数的性能表现与 CPU 架构、系统调度、音频内容和运行负载有关，建议使用相同 WAV 文件进行多次对比测试。

### 11.3 性能评价指标

建议重点记录以下指标：

| 指标 | 说明 |
|---|---|
| 单文件推理耗时 | 单次 Whisper 推理所需时间 |
| 平均推理耗时 | 多个 WAV 文件的平均识别时间 |
| 最大推理耗时 | 识别过程中的最慢一次推理 |
| 任务队列长度 | 尚未完成识别的文件数量 |
| 识别准确率 | 识别文本与实际语音内容的匹配程度 |

对于每 3 秒产生一个 WAV 的应用场景，如果希望任务队列不持续积压，平均处理时间需要低于 3 秒，并且还需要考虑文件检查、预处理及系统调度开销。

---

## 12. 常见问题

### 12.1 找不到模型文件

检查模型文件是否存在：

```bash
ls -lh WhisperModule/ggml-base.bin
```

同时检查源码中配置的模型路径是否正确。

### 12.2 编译时提示找不到 sndfile

安装依赖：

```bash
sudo apt install libsndfile1-dev
```

### 12.3 编译时提示找不到 samplerate

安装依赖：

```bash
sudo apt install libsamplerate0-dev
```

### 12.4 程序无法监听目录

检查目录是否存在：

```bash
ls -ld Wav
```

检查当前用户是否具有目录读取和写入权限。

### 12.5 识别结果为空或不准确

可以检查：

1. WAV 文件是否包含有效语音。
2. 音频是否能够正常播放。
3. 采样率和声道信息是否正确读取。
4. 音频重采样是否成功。
5. 模型文件是否正确。
6. 中文语言参数是否设置为 `zh`。

### 12.6 识别任务出现积压

程序采用顺序任务处理方式。如果 WAV 文件产生速度持续高于识别处理速度，待处理任务队列就会不断增加。

建议通过推理耗时统计、任务队列长度和 CPU 运行状态进行分析。

---

## 13. 当前实现特点与限制

当前版本主要面向固定目录下的 WAV 文件自动识别任务，具有以下特点：

**已经实现：**

- Linux 文件系统事件监听。
- WAV 文件自动发现。
- 文件稳定性检查。
- 时间戳排序。
- 任务去重与处理记录。
- 音频读取和预处理。
- Whisper 中文语音识别。
- 识别文本保存。
- 推理耗时统计。

**当前实现的限制：**

- 识别任务采用单个工作线程顺序处理。
- 当前主要面向 WAV 文件，不直接处理其他音频格式。
- 识别性能受模型规模、CPU 计算能力及系统负载影响。
- 目前没有实现多任务并行识别。
- 识别准确率受音频质量、噪声和语音内容影响。

---

## 14. 后续改进方向

后续可以根据实际应用需求逐步完善：

1. 增加识别任务队列状态监控。
2. 增加文件异常和识别失败后的重试机制。
3. 优化任务调度及音频预处理流程。
4. 支持识别结果按文件独立保存。
5. 增加运行日志和性能统计文件。
6. 增加系统运行状态监测。
7. 根据实际设备性能优化模型推理参数。

---

## 15. 许可证与致谢

本项目使用以下开源组件：

- [whisper.cpp](https://github.com/ggml-org/whisper.cpp)：语音识别推理框架。
- [libsndfile](https://github.com/libsndfile/libsndfile)：音频文件读取库。
- [libsamplerate](https://github.com/libsndfile/libsamplerate)：音频重采样库。

使用和分发相关组件时，请遵守各自项目的开源许可证。

---

## 16. 联系与反馈

如果在项目编译、部署或使用过程中遇到问题，可以通过 GitHub Issues 提交问题，并尽量附上：

- 操作系统及硬件平台。
- 编译器和 CMake 版本。
- 完整错误日志。
- WAV 文件的采样率、声道数和时长。
- 程序运行日志。

欢迎提交问题和改进建议。