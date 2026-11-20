# A7C2 Pixel Shift / 摇摇乐

> [!WARNING]
> **本项目为非官方、独立开展的相机技术研究，与索尼不存在隶属、授权或推荐关系。修改设备可能造成无法启动、反复重启、功能失常或数据丢失。即使写入和读回校验通过，也不代表能够安全启动；恢复方案同样可能失败。**
>
> **项目免费提供，原创部分仅允许非商业用途。禁止利用本项目及其改版开展收费代刷、付费远程安装、工具转售、收费解锁、激活码或加密狗销售，以及硬件捆绑变现等业务。作者不承接有偿改机，也未授权第三方以作者名义提供收费服务。**
>
> **不得冒充索尼官方、项目作者或所谓授权服务商。第三方收款、宣传或保修承诺均不构成作者的参与、认可或担保。**
>
> 仅限本人合法持有，或已得到机主明确许可的设备。请事先备份重要资料，并自行评估设备损坏、维修及保修受影响的风险；无法接受这些后果时，请勿继续。
>
> **操作前请完整阅读：[设备风险与使用约定](docs/notices.md)。原创代码的授权范围见 [LICENSE](LICENSE)。**

Sony A7C II（ILCE-7CM2）像素位移解锁研究，以及 **16 帧 → 131 MP JPEG 的机内合成源码**。

**真实机身记录：完整合成、写卡、完整读回和发布为 16.5295185 秒，四舍五入 16.530 秒。**

English: Research code for Sony A7C II pixel-shift software gates and an on-camera 16-frame, 131 MP JPEG pipeline. The recorded 16.530 s includes synthesis, SD writing, full readback and publication. This is a firmware-specific reference implementation, not a general-purpose one-click camera application.

## 项目包括什么

| 内容 | 文件 | 状态 |
| --- | --- | --- |
| C2 摇摇乐解锁原理、既有研究与加载协议 | [docs/unlock.md](docs/unlock.md) | 包含菜单、支持表、重定位、快照和实机反馈的证据边界 |
| 严格校验的离线解锁／恢复工具 | [tools/patch_unlock.py](tools/patch_unlock.py) | 支持指定 2.01 原始 App 和配套 WBI 镜像；不连接相机 |
| 16.530 秒版本的机内完整合成 | [native/](native/) | 原始处理代码，仅整理 include 路径；跨目录源码依赖已收齐 |
| AArch64 交叉编译与 Python 相机调用 | [tools/build.py](tools/build.py)、[tools/run_camera.py](tools/run_camera.py) | 依赖用户本地 A7C2Tools 和指定 Turbo 构建 |
| 计时、CRC、配置与来源 | [evidence/](evidence/) | 从原始机内日志提取的公开记录，不包含私有完整日志 |

本项目为**源码公开、仅限非商业用途**的研究工程，原创部分适用 [项目非商业使用许可](LICENSE)。由于包含非商业限制，它不属于 [OSI 定义的开源软件](https://opensource.org/osd)。第三方部分仍按各自原有许可使用，详见 [NOTICE](NOTICE)。不分发 Sony 固件、启动快照、照片、SDK、驱动或个人机身标识。

English licensing note: Original project components are source-available for noncommercial use under the accompanying LICENSE, not MIT or an OSI-approved open-source license. Third-party components keep their own licenses. This is independent, unofficial research with no Sony affiliation or endorsement; device modification and recovery may fail.

## 16.530 秒版本

| 配置 | 数值 |
| --- | --- |
| 输入 | 同组 16 张无压缩 U16 ARW，7040×4688，14 bit |
| 输出 | 14016×9344，约 131 MP，JPEG Q90、4:2:2 |
| 读取／重建条带 | 256 行／256 行 |
| RAW 工作槽／RGB8 槽 | 3／8 |
| 线程 | 文件、重建、辅助计算、JPEG，共 4 个 |
| JPEG | 原 AArch64 libjpeg-turbo 3.1.4.1，一次交 16 行 |
| 只读句柄复用 | 8 个 |
| RAW 地址／文件偏移对齐 | 4096 字节／512 字节 |
| 写出 | JPEG 主体单次写入，EXIF／MPF 回填，Flush，完整读回，发布 |
| 回放预览／缩略图 | MPF 1616×1080／160×120 |
| 原生保留池 | 未使用；无需 580 MiB RAW 常驻缓存 |

四个线程并行，分项计数不能直接相加。按墙钟互斥的阶段：

| 阶段 | 秒 |
| --- | ---: |
| 准备 | 1.2926295 |
| 并行流水线 | 13.9730440 |
| 收尾（含 Flush、完整读回和发布） | 1.2636110 |
| 最后资源回收 | 0.0002340 |
| **总计** | **16.5295185** |

相同任务此前为 33.471989 秒，本次减少约 50.62%。另一轮为 16.3971855 秒；本项目以用户指定的 **16.530 秒记录**为基线，不把两次测试当作大量重复测试的中位数。详见 [性能和读取根因](docs/performance.md)。

## 开始使用

1. 先阅读 [设备风险与使用约定](docs/notices.md) 和 [解锁方法](docs/unlock.md)。离线工具只生成单独的镜像副本，不能当成 Sony 官方升级包刷入。
2. 按 [构建说明](docs/build-and-run.md) 准备 Zig 0.13.0、AArch64 Turbo 静态库和本地 A7C2Tools 3.03。
3. 执行源码检查与编译。编译不会连接相机。
4. 机内调用需要自己的机身绑定、运行库校验和私有曲线文件；只有显式执行相机脚本才会连接和合成。

```console
python -m unittest discover -s tests -v
python tools/audit_public.py
python tools/patch_unlock.py inspect --image app --input /path/to/appFw.so
python tools/build.py --zig /path/to/zig --jpeg-dir /path/to/official-arm64
```

## 当前边界

- **本版固定为历史参考数据组**：`101MSDCF/DSC03044.ARW`～`DSC03059.ARW`，组号、白平衡、黑白电平、逐帧源 CRC、线性 CRC 和 JPEG 主体 CRC 都有固定校验。其他照片会被拒绝。它不是自动扫描任意 16 张照片的通用产品。
- 历史基线输出到新的 `173MSDCF/DSC00001.JPG`，目录已存在时拒绝执行。后续“追加到已有目录末尾”的实现不在这个计时基线内。
- **131 MP 成片机内放大仍失败**。同一份合成像素下采样到 7008×4672 的对照图，经用户确认可以放大；不能把该对照说成 131 MP 放大已修复。
- **16.530 秒基线没有后续 DISP 修复**。后续保留 Sony MakerNote／HiddenData 的版本耗时 16.8147375 秒，用户已确认 DISP 恢复；本项目记录其结果，不把该修复混入 16.530 秒版本。
- 没有可见的机身合成文字或进度条。原生提示测试未通过用户可见性验收，相关尝试未集成。
- 解锁与合成是两个环节。旧研究报告过“保持摇摇乐开启关机，再开机报错”；当前资料不足以标记为已修复。旧规避记录是关机前将摇摇乐置 OFF。
- 解锁补丁严格限定指定固件哈希。已修改 FL2／FL3、LUT 或其他内容的 App 不适用原始固件补丁。合成测试机的 App 已有修改，其哈希与原始解锁候选不同。

## 贡献

提交问题时请提供固件版本、公开模块哈希、条带／队列配置和脱敏分项计时。不要上传固件、RAW、序列号、完整 USB 快照或私有校准文件。改动应说明是否改变像素、JPEG 主体、EXIF、发布位置或线程退出行为。离线与机内测试结果请分开标注。
