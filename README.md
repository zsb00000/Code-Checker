
# Parallel Code Judge (并行代码对排器)

基于 C++ 多线程后端 + Python Flask 前端的代码评测系统，支持对拍测试（Make-Ans-Unknown 模式），适用于 Windows 平台。

## 功能特性

- **多线程并行评测**：支持同时运行 k 组（k < 50）评测任务，充分利用多核 CPU
- **严格的资源隔离**：每个评测任务使用独立临时目录，互不干扰
- **差异化资源限制**：
  - `unknown.cpp`：限制 2 秒 CPU 时间，256 MB 内存
  - `ans.cpp`：不限制时间/内存（仅设置 60 秒/4GB 防崩溃保护）
- **智能文件比对**：自动忽略行尾空格和文件末尾空行
- **Web 可视化界面**：基于 Flask 的友好前端，实时显示进度和结果
- **健壮性保障**：文件写入同步检测、句柄隔离、重试机制，避免 Windows 文件系统竞争

## 项目结构


```
Code-Checker/
├── include/
│   └── ThreadPool.h              # 线程池头文件（C++11）
├── src/
│   ├── backend/
│   │   ├── judge_parallel.cpp    # C++ 评测后端源码
│   │   └── judge_parallel.exe    # 编译后的可执行文件（需自行编译）
│   └── frontend/
│       ├── app.py                # Flask Web 服务器
│       └── templates/
│           └── index.html        # 前端页面
└── README.md
```

## 环境要求

- **操作系统**：Windows 7/10/11 (x64)
- **编译器**：MinGW-w64 或 MSYS2（提供 g++ 命令）
- **Python**：Python 3.7+（推荐 3.9+）
- **依赖库**：Flask (`pip install flask`)

## 安装部署

### 1. 编译 C++ 后端

确保 `g++` 已加入系统 PATH，然后在 `src/backend` 目录下执行：

```powershell
cd src/backend
g++ -O2 -std=c++17 -pthread -o judge_parallel judge_parallel.cpp
```

编译成功后，会生成 `judge_parallel.exe`。

### 2. 安装 Python 依赖

```powershell
cd src/frontend
pip install flask
```

### 3. 运行服务

```powershell
python app.py
```

服务默认运行在 `http://127.0.0.1:5000`，使用浏览器访问即可。

## 使用说明

### 准备文件

准备三个 C++ 源文件：

1. **make.cpp**（数据生成器）
   - 无输入，输出测试数据到 stdout
   - 示例：生成随机数、构造图论数据等

2. **ans.cpp**（标准答案/暴力解法）
   - 从 stdin 读取 make.cpp 的输出
   - 输出正确答案到 stdout
   - **不限制运行时间和内存**（除非导致系统崩溃）

3. **unknown.cpp**（待验证代码）
   - 从 stdin 读取 make.cpp 的输出
   - 输出待验证答案到 stdout
   - **限制 2 秒运行时间，512MB 内存**

### Web 界面操作

1. 打开浏览器访问 `http://127.0.0.1:5000`
2. 分别上传三个文件：`make.cpp`、`ans.cpp`、`unknown.cpp`
3. 设置运行次数 **k**（1-49 的整数，推荐 10-20 次用于对拍）
4. 点击"开始并行评测"

### 结果说明

- **AC (Accepted)**：unknown.cpp 输出与 ans.cpp 完全一致（忽略行尾空格）
- **WA (Wrong Answer)**：输出存在差异
- **UKE (Unknown Error)**：
  - 编译错误
  - 运行时错误（段错误、除零等）
  - 超时（TLE，仅针对 unknown.cpp）
  - 内存超限（MLE，仅针对 unknown.cpp）

## 评测流程

对于每一次评测任务（共 k 次）：

1. **创建独立工作目录**（`%TEMP%\task_{id}_{random}\`）
2. **复制**三个源文件到工作目录
3. **编译**（串行执行）：
   - `g++ -O2 -std=c++17 -o make.exe make.cpp`
   - `g++ -O2 -std=c++17 -o ans.exe ans.cpp`
   - `g++ -O2 -std=c++17 -o unknown.exe unknown.cpp`
4. **运行**（串行执行，确保文件同步）：
   - 运行 `make.exe` 生成 `out1`（测试数据）
   - 运行 `ans.exe < out1 > std`（标准输出）
   - 运行 `unknown.exe < out1 > out2`（待测输出，带资源限制）
5. **比对**：逐行比较 `std` 和 `out2`，忽略行尾空格和文件末空行
6. **清理**：删除临时目录

**注意**：k 个任务之间是并行的（通过线程池），但单个任务内部的编译和运行是串行的，避免 Windows 文件句柄竞争。

## 技术细节

### 并发控制
- 使用开源库 `ThreadPool`（基于 C++11 `std::thread` 和 `std::future`）
- 默认线程数：`min(k, 4)`，避免 Windows 句柄资源耗尽

### 文件同步机制
- 写入文件后强制 `flush()` 和 `close()`
- 运行程序前检测输入文件是否可独占打开（确保写入完成）
- 程序结束后检测输出文件大小稳定性（确保落盘）

### 资源限制实现
- **时间**：通过 `WaitForSingleObject` 超时检测 + `TerminateProcess` 终止
- **内存**：使用 Windows Job Object API 设置硬内存限制（512MB）

### 安全性
- 每个任务独立临时目录，防止文件冲突
- 使用 `cmd.exe /c` 进行 IO 重定向，避免父子进程句柄继承竞争
- 自动清理临时文件（即使评测崩溃）

## 注意事项

1. **杀毒软件**：Windows Defender 或其他杀毒软件可能拦截编译器进程或临时文件，建议将项目目录加入白名单，或首次运行时允许权限。
2. **路径问题**：确保 `g++` 已加入系统 PATH，且路径中不含中文或特殊字符。
3. **并发数**：如果系统出现随机 UKE，可在 `app.py` 中将线程数改为 1（完全串行），或降低并发数。
4. **随机数种子**：`make.cpp` 中使用 `srand(time(0))` 时，同一秒内启动的任务会生成相同数据（不影响正确性，仅影响测试覆盖率）。

## 故障排查

- **编译失败（UKE）**：检查 g++ 是否安装，或查看临时目录中的 `compile_err.txt`
- **运行时错误（UKE）**：检查是否数组越界、栈溢出（Windows 默认栈较小）
- **随机 WA/UKE**：通常是 Windows 文件系统延迟导致，如出现可以降低并发数或增加 `Sleep` 延迟

## License

MIT License