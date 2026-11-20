# 构建与机内调用

本仓库是指定实测版本的研究复现工程。构建不连接相机；`run_camera.py` 才会显式连接相机、读卡和创建新的成片。

## 构建依赖

- Python 3.12 或兼容版本；相机调用使用 Windows 的 `msvcrt` 设备锁。
- Zig **0.13.0**；目标 `aarch64-linux-gnu.2.25`。
- 用户本地的 A7C2Tools **3.03**（不随项目分发）。
- AArch64 libjpeg-turbo **3.1.4.1** 的 `libjpeg.a` 和匹配的 `jpeglib.h/jconfig.h/jmorecfg.h/jerror.h`。
- `python -m pip install -r requirements-dev.txt`，供 ELF 静态检查使用。

历史 Turbo 静态库 SHA-256：

```text
b2db824adb96ed9f4c1864caff2ae5dae92bac135359f5ce3e8b4b2888e0138a
```

原研究将这组库和头文件保存为 `official-arm64`。本项目没有重新分发这个依赖，也不宣称任意自行构建的同版本库具有相同字节哈希。使用其他构建的 Turbo 需要单独验证 SIMD、TLS、RGB8／JPEG 主体 CRC 和性能；当前基线构建脚本会拒绝不同的静态库。上游项目为 [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo)，许可副本在 [third_party](../third_party/libjpeg-turbo/LICENSE.md)。

```console
python tools/build.py --zig /path/to/zig --jpeg-dir /path/to/official-arm64 --dcf 173
```

产物是忽略目录中的 `build/camera-merge.so` 和 `build/build.json`。构建后检查 AArch64、共享对象类型、不可写的可执行段、无 text relocation、一个初始化入口，以及模块／源码哈希。`--dcf` 仅选择历史方案的新输出目录编号，不改变合成参数。

整理后源码已在本地重新交叉编译，得到 **432,528 字节**，SHA-256 与 16.530 秒原模块完全一致：

```text
9bca0b29700446f503027e6b60347623deb27dd48e823619d9af0cd50f654e87
```

这次是构建验证，没有重跑相机或电脑性能测试。证据见 [reproduction.json](../evidence/reproduction.json)。使用不同工具链、静态库或 DCF 编号可能改变模块哈希，不代表自动获得相同计时。

## 机身绑定与私有文件

将 [reference.example.json](../profiles/reference.example.json) 复制为 `private/profile.private.json`。其中两个空的机身身份摘要必须来自你自己的 A7C2Tools 同机验证记录：

- `usb_serial_sha256`：USB 层的同机身份摘要。
- `identity.serial_sha256`：service identity 层的同机身份摘要。

不要把序列号填成明文，也不要用另一个机身的摘要。公开模板中的 App／CP／Fsys／运行库哈希来自实测软件组合；只适用于相同版本和修改状态。新的组合需要重新建立和审查合同，不能仅删除哈希判断。

本版还要求历史的 792 字节 `PSCLR001` 曲线文件，SHA-256：

```text
5b84ac2dfac24713448b942a139d9f706b747280854d1340b0cfccda52ee5646
```

该文件和原始照片均为使用者的私有测试材料，仓库没有附带。参考数据组的白平衡、矩阵、黑白电平和 CRC 固定在源码中；缺少这些材料时，可以构建、核查解锁方法和分析计时，不能把任意 ARW 当作该轮复现输入。

## 显式执行一次机内完整合成

```console
python tools/run_camera.py --tools /path/to/A7C2Tools/_internal --profile private/profile.private.json --calibration private/camera-color-params.bin
```

流程包括：设备锁 → 同机身份、App／CP／Fsys／运行库校验 → 检查临时文件不存在 → 暂存模块和曲线、逐字节读回 → 执行一次 → 收回结果／分项时间线 → 校验源、线性、JPEG 主体和完整读回 CRC → 清理本任务文件 → 确认固件未改动 → 恢复并核对普通 USB。

源文件固定为 `101MSDCF/DSC03044.ARW` 至 `DSC03059.ARW`。默认创建 `173MSDCF/DSC00001.JPG`；目标目录已存在会拒绝。这个路径是历史基线行为，并不是后续追加到图库末尾的实现。

相机脚本不会写 App、CP 或相机设置；它会在 SD 卡上写自己的合成文件，并在 Linux 临时目录执行自己的模块。异常时保留证据，不自动重跑一轮。完整机身记录、时间线、身份和输出路径均保存在被 Git 忽略的 `private/runs/`。

## 离线检查

```console
python -m unittest discover -s tests -v
python tools/audit_public.py
python tools/run_camera.py --help
```

CI 只做离线校验，不假装有相机，也不分发或下载 Sony 固件。对于新的输入支持、现有目录追加、DISP 修复和进度提示，应建立独立变更和机内验收记录，保留这个可核对的历史基线。
