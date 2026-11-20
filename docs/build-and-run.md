# 独立构建与集成边界

本仓库提供离线解锁研究、机内合成原生源码和独立交叉编译脚本。不提供 USB 连接、设备安装器或机内加载脚本；下列命令均不连接相机。

## 构建依赖

- Python 3.12 或兼容版本。
- Zig **0.13.0**；目标 `aarch64-linux-gnu.2.25`。
- AArch64 libjpeg-turbo **3.1.4.1** 的 `libjpeg.a` 和匹配的 `jpeglib.h/jconfig.h/jmorecfg.h/jerror.h`。
- `python -m pip install -r requirements-dev.txt`，供 ELF 静态检查使用。

历史 Turbo 静态库 SHA-256：

```text
b2db824adb96ed9f4c1864caff2ae5dae92bac135359f5ce3e8b4b2888e0138a
```

本项目不分发此静态库，也不宣称自行构建的同版本库具有相同哈希。当前构建脚本严格校验该库；其他构建需要单独验证 SIMD、TLS、RGB8／JPEG 主体 CRC 和性能。上游项目为 [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo)，许可副本在 [third_party](../third_party/libjpeg-turbo/LICENSE.md)。

```console
python -m pip install -r requirements-dev.txt
python tools/build.py --zig /path/to/zig --jpeg-dir /path/to/official-arm64 --dcf 173
```

产物位于被 Git 忽略的 `build/camera-merge.so` 和 `build/build.json`。脚本检查 AArch64、共享对象类型、不可写的可执行段、无 text relocation、一个初始化入口，以及模块／源码哈希。`--dcf` 仅选择历史方案的新输出目录编号，不改变合成参数。

## 历史基线与当前源码

16.530 秒记录对应的历史模块为 **432,528 字节**，SHA-256：

```text
9bca0b29700446f503027e6b60347623deb27dd48e823619d9af0cd50f654e87
```

清理前曾独立交叉编译并复现该模块哈希。当前源码已移除外部工具接入：运行文件使用项目自己的 `/tmp/a7c2-pixelshift/` 目录，模块不再删除加载器文件。处理算法、线程配置及合成参数保持原样，但模块哈希会改变。历史构建复现与当前构建检查分别记录在 [reproduction.json](../evidence/reproduction.json)。当前修改未重新进行机内测试，不能据此宣称重新达到 16.530 秒。

## 原生模块集成约定

模块保留标准 ELF 初始化入口，加载即执行一次固定数据组合成。仓库没有实现设备连接和模块加载；下列内容是原生源码的输入输出约定，不是可直接执行的相机安装流程。

集成方负责核验设备身份、固件及运行库版本、模块和私有输入哈希；负责在加载前创建本任务独占的临时目录、确认旧结果不存在，并在执行结束后回收文件和卸载模块。不能将旧结果当成本次成功，也不能在处理线程仍运行时删除缓冲或卸载模块。

| 路径 | 用途 |
| --- | --- |
| `/tmp/a7c2-pixelshift/ps-color-calibration.input` | 加载前提供的 792 字节曲线输入 |
| `/tmp/a7c2-pixelshift/a7c2.probe.result` | 二进制结果，由 [tools/report.py](../tools/report.py) 解析 |
| `/tmp/a7c2-pixelshift/ps-j3-three.trace` | 分项时间线，由 [tools/three_trace.py](../tools/three_trace.py) 解析 |
| `/tmp/a7c2-pixelshift/ps-native-cache.trace` | 原生缓存诊断；当前基线配置未使用原生 RAW 常驻缓存 |

历史 `PSCLR001` 曲线文件 SHA-256：

```text
5b84ac2dfac24713448b942a139d9f706b747280854d1340b0cfccda52ee5646
```

该曲线和原始照片是私有测试材料，仓库没有附带。参考数据组固定为 `101MSDCF/DSC03044.ARW` 至 `DSC03059.ARW`，白平衡、矩阵、黑白电平和 CRC 固定在源码中。缺少这些材料时可以构建和分析源码，不能用任意 ARW 替代。

默认输出 `173MSDCF/DSC00001.JPG`，目标目录已存在时拒绝执行。整链包括合成、写卡、EXIF／MPF 回填、Flush、完整读回及发布；源 CRC、线性 CRC、JPEG 主体及完整读回 CRC 继续保留。固件内 Fsys 函数地址和签名仍然依赖历史实测版本，不能将此源码视为通用机内应用。

## 离线检查

```console
python -m unittest discover -s tests -v
python tools/audit_public.py
python tools/build.py --help
```

CI 只做离线校验，不下载固件或执行相机任务。新的输入支持、现有目录追加、DISP 修复和进度提示需另行建立机内验收记录。
