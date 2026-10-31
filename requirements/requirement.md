# RAW 色彩还原修图软件需求文档（GPLv3｜C++ Qt6 Widgets）

文件名：`requirements.md`

> 
> 用途：交付 Claude Code 开发；定位：桌面优先、性能体验优先、开源 GPLv3、支持静态链接打包、特色为可调 RAW 底层解码参数 + 拜耳域 CCM 自定义矩阵；远期可复用核心管线做手机端（移动端不在 MVP 内）

## 1. 整体目标与设计原则

### 1.1 产品目标

读取主流相机 RAW 文件，构建 32 位浮点线性光专业色彩管线，默认取向为中性、低美化、忠于场景真实色彩；
双操作模式：简易调色模式（普通用户） / RAW 底层进阶校准模式（硬核色彩玩家，可修改解码参数、自定义 CamRGB→XYZ CCM 矩阵）；
非破坏性编辑：绝不修改原始 RAW，所有调整参数独立序列化保存；
支持导出标准带 ICC 配置的图像文件；
项目开源协议：**GPLv3**，源码公开，允许静态链接依赖，目标支持单文件打包分发。

### 1.2 技术栈

- 编程语言：C++17
- UI 框架：Qt6 Widgets（**禁止 QML**），使用 Qt GPL 版本
- 底层库：libraw、lcms2、lensfun
- 并行加速：OpenMP
- 构建系统：CMake
- GPU 预留：Qt OpenGL/Vulkan 预览接口（MVP 仅 CPU 渲染，接口预留，不实现 GPU 管线）

依赖许可清单：

- Qt6 (GPL)
- libraw (LGPL 2.1，与 GPLv3 兼容)
- lcms2 (MIT)
- lensfun (LGPL 3.0，与 GPLv3 兼容)

### 1.3 核心设计约束（硬性，代码必须遵守）

1. 严格区分**线性光空间 / Gamma 空间**；所有底层光影、矩阵、调色运算必须在 32bit 浮点线性域；禁止将 sRGB/Gamma 图像混入色彩计算管线。
2. 内部固定工作色彩空间：ProPhotoRGB；预览、导出由 lcms2 负责色彩转换，读取显示器 ICC 适配屏幕。
3. 拜耳域 CCM 3×3 矩阵是**线性变换**，不等同于 3D LUT；二者在管线中位置独立，不要混淆。
4. 渲染策略：拖动交互滑块时渲染低分辨率预览图保证流畅；松开滑块 / 确认参数后，再执行全尺寸原图渲染。
5. 全部可调参数支持序列化、保存、加载、导入导出。
6. 不硬编码厂商色彩偏好；相机原生矩阵仅作为默认值，用户可以完全覆盖。
7. CCM 矩阵合法性校验，行列式异常、极端数值给出警告，防止画面崩坏。
8. 模块解耦：渲染管线逻辑和 UI 代码分离；管线核心独立，为远期移动端复用预留可能性。

### 1.4 支持格式

- RAW：DNG、CR2、CR3、NEF、ARW、RW2、RAF（libraw 原生支持）
- 参数工程文件：`.rawedit.json`（图片调整工程）
- 相机校准档案：`.camcal`（底层 CCM + 解码预设）
- 导出图片：16-bit TIFF、8-bit JPG、WebP，文件嵌入 ICC 配置文件
- 预留接口：`.cube` 3D LUT 加载（MVP 不实现，仅预留管线结构）

## 2. 渲染管线固定顺序（代码严格遵循）

```
原始RAW拜耳数据
→ [RawLoader：libraw读取]
→ 黑电平扣除（用户可叠加偏移）
→ 白电平饱和裁剪
→ 【可选分支：进阶模式启用拜耳域CCM矩阵线性变换】
→ 基础坏点修复
→ Demosaic去拜耳（算法可选+摩尔纹平滑参数）
→ lensfun镜头校正：畸变、暗角、轴向色差（可开关、参数可手动覆盖）
→ CamRGB → XYZ转换（未启用拜耳CCM时，使用RAW自带原厂矩阵）
→ XYZ → ProPhotoRGB（lcms2）
→ 白平衡白点缩放：色温+色调、灰点拾取
→ 光影调整：曝光、高光压缩、阴影提升、对比度、去雾
→ HSL调色：红橙黄绿青蓝紫通道偏移
→ 单通道曲线（R/G/B/L）
→ 【预留分支：叠加cube 3D LUT，混合强度控制，MVP不实现】
→ 降噪：明度降噪、色度降噪
→ USM锐化，带边缘掩码，避免噪点放大
→ 色域映射 + 目标色彩空间转换（sRGB / AdobeRGB）
→ Gamma编码
→ 预览画布 / 导出文件
```

优先级规则：拜耳域自定义 CCM > RAW 内置原厂 cam2xyz 矩阵；白平衡在 XYZ 转 ProPhoto 之后。

## 3. 模块拆分

### 模块 A RawLoader（RAW 解码封装层）

封装 libraw，隔离上层业务；输出拜耳浮点数组、元数据、原厂 cam2xyz 矩阵、黑白电平、拜耳模式。
可配置项（进阶模式开放）：

- black_level：黑电平叠加偏移
- white_level：饱和电平上限
- force_bayer_pattern：强制拜耳模式，自动识别优先
- disable_camera_matrix：bool，禁用原厂矩阵
- demosaic_algorithm：枚举 BILINEAR / VNG4 / AMAZE / LMMSE
- demosaic_smooth：0.0 ~ 1.0，摩尔纹抑制强度
- raw_crop：传感器边缘裁剪坐标

输出：浮点拜耳 buffer、元数据字典、解码状态码。

### 模块 B ColorCalibration CCM 引擎（特色核心模块）

1. 存储 3×3 浮点矩阵 cam2xyz
2. 实现拜耳域逐像素线性矩阵乘法
3. 校验函数：行列式检测、极端数值检测，非法矩阵弹窗警告
4. 能力：重置原厂矩阵、9 参数滑块 + 输入框手动修改、导入 CSV、导出 CSV、读写`.camcal`
5. 预留接口：从 24 色卡采样自动求解 CCM（UI 隐藏，仅函数占位，MVP 不做）

`.camcal` 结构示例

```
{
  "version": "1.0",
  "camera_model": "Sony A7M4",
  "black_level": 128,
  "white_level": 16383,
  "demosaic_algo": "AMAZE",
  "demosaic_smooth": 0.25,
  "cam2xyz": [
    [1.02, -0.07, 0.03],
    [-0.05, 0.98, 0.02],
    [0.01, -0.03, 1.04]
  ],
  "notes": "自定义中性校准"
}
```

### 模块 C ColorManagement 色彩管理引擎

封装 lcms2

- 内置 ICC：ProPhotoRGB、sRGB、AdobeRGB
- 转换接口：XYZ ↔ ProPhotoRGB；工作空间 ↔ 显示器空间；工作空间 ↔ 导出目标空间
- 读取系统显示器 ICC，预览适配屏幕色域
- 渲染意图：相对比色优先

### 模块 D EditContext 参数模型（非破坏性编辑）

全部调整参数序列化保存为 json，不改动 RAW

```
raw_settings: RawLoader全部解码参数
ccm_settings: 是否启用自定义矩阵、cam2xyz数组
lens_correction: enable、畸变强度、暗角强度、色差开关
white_balance: temperature, tint, gray_pick_point(x,y)
light: exposure, highlight, shadow, contrast, dehaze
hsl: 红橙黄绿青蓝紫，色相/饱和度/明亮度偏移
curve: R/G/B/L通道控制点数组
lut: enable, path, opacity（预留）
detail: noise_luma, noise_chroma, sharpen_amount, sharpen_radius
```

### 模块 E PipelineRenderer 渲染管线

输入：RawLoader 结果 + EditContext；输出浮点 RGB 图像

- 严格按管线顺序实现
- 支持两种尺寸：preview 小图交互、full_size 原图导出
- OpenMP 多线程并行
- 预留 GPU 渲染接口，MVP 仅 CPU 实现
- 进度回调、错误回调（不支持 RAW、内存不足、矩阵非法等）

### 模块 F UI 层（Qt Widgets）

全局开关：【简易模式】｜【RAW 底层校准模式】

#### F1 简易模式（默认打开）

- 左侧：文件列表、RAW 打开
- 中间预览画布：适合窗口、1:1 原始比例、放大吸管取灰点
- 右侧面板：白平衡、光影、HSL、细节、导出
- 底层解码 & CCM 面板隐藏、只读不可编辑

#### F2 RAW 底层校准模式（解锁高级标签页：传感器 & 色彩映射）

控件清单：

1. 传感器组：黑电平、白电平输入 + 滑块、强制拜耳模式下拉
2. CCM 矩阵组：3 行 ×3 列输入框 + 配套滑块；按钮：重置原厂、导入 CSV、导出 CSV；矩阵非法警告
3. Demosaic 组：算法下拉、平滑抑制滑块
4. 镜头校正组：lensfun 自动校正开关、手动覆盖畸变 / 暗角 / 色差滑块
5. 校准预设：加载 / 保存 `.camcal`

#### F3 通用 UI 控件

- 原图 / 当前效果分屏对比
- 参数快照：多组参数快速切换对比
- 白平衡灰点吸管
- 线性域直方图（非 sRGB 直方图）
- 导出弹窗：格式、位深、目标色彩空间、质量设置
- 日志面板：解码警告、不支持文件、矩阵校验告警
- 最近打开文件列表

### 模块 G IO & 序列化

1. 工程保存：`.rawedit.json`
2. `.camcal` 读写
3. 导出图片，嵌入 ICC profile
4. 日志输出

## 4. MVP 明确排除项（不要实现）

❌ 局部调整：渐变滤镜、径向滤镜、蒙版画笔
❌ 批量导出队列
❌ AI 降噪、AI 色彩、AI 修复
❌ 视频 RAW、HEIF
❌ 自定义 Demosaic 内核
❌ 插件系统
❌ 云同步、账号系统
❌ 暗帧 / 平场校正（预留接口，二期）
❌ 完整色卡自动计算 CCM（仅预留函数）

## 5. 性能与质量要求

1. 大图渲染放到子线程，UI 全程不阻塞；
2. 管线内部全部 float32；仅导出阶段降精度；
3. 内存管理：大图 buffer 及时释放，避免持续内存上涨；
4. 所有色彩计算强制线性域；
5. 支持 4000 万像素级 RAW 稳定渲染；
6. 编译目标支持静态链接全部依赖，打包单 exe。

## 6. 错误处理规范

- RAW 打开失败：区分文件损坏、不支持格式、libraw 解码异常，友好提示
- CCM 矩阵非法：标红警告，不强制回滚，提示色彩风险
- 内存不足：提示降低预览分辨率或者缩小导出尺寸
- lensfun 无匹配镜头数据：自动关闭镜头校正，文字提示

## 7. 预留扩展（仅接口 / 结构体，MVP 不编码实现）

- `.cube` 3D LUT 加载与强度混合
- 暗帧、平场噪声校正
- 色卡自动求解 CCM
- GPU Compute 管线加速
- 批量导出

## 8. 版权声明模板（打包内置 NOTICE.txt/ 软件关于页）

```
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License Version 3 as published by the Free Software Foundation.

This software uses following libraries:
Qt6 (GPL)
libraw (LGPL 2.1)
lensfun (LGPL 3.0)
LittleCMS2 / lcms2 (MIT)
```

## 编译、链接与打包策略（自适应模式）
本项目CMake脚本**不硬编码vcpkg triplet、不强制CRT类型**，通过 MSVC_RUNTIME_LIBRARY 自动跟随vcpkg三元组配置，两种构建模式共用同一套源码：

- 模式1：开发调试模式（动态链接）
  vcpkg triplet：x64-windows
  Qt使用动态版本，CRT = /MD
  产物：exe + 配套dll；使用windeployqt完成依赖部署；编译速度快，用于功能开发、管线调试、UI迭代，不作为对外发行包。

- 模式2：正式发行模式（全静态，目标单exe）
  vcpkg triplet：x64-windows-static
  Qt6 使用GPL版本、静态编译，CRT = /MT，所有依赖（Qt/libraw/lcms2/lensfun）统一静态链接、统一CRT。
  要求：静态编译Qt时，内置图片插件、Windows平台插件，不依赖外部qt插件dll；Release开启LTO优化，压缩最终exe体积。
  产物：独立单exe，双击直接运行，适合GitHub Release分发。

❌ 禁止混合模式（vcpkg静态 + Qt动态）：极易出现CRT不匹配、跨堆内存崩溃，CMake增加检查逻辑，检测到CRT不一致时抛出致命提示，终止构建。

构建说明分开提供两套shell命令，分别对应上面两种模式。

### ICC / 色彩配置文件策略
固定输出/工作空间（sRGB、AdobeRGB、ProPhotoRGB）**全部由lcms2在代码内通过标准常量动态生成，不捆绑外部*.icc文件**，便于构建独立单exe：
1. sRGB：直接调用lcms2内置 cmsCreate_sRGBProfile()
2. AdobeRGB(1998)：代码写入标准原色、白点、Gamma=2.2，使用 cmsCreateRGBProfile 构造
3. ProPhotoRGB (ROMM RGB)：代码写入标准原色、D50白点、Gamma=1.8，使用 cmsCreateRGBProfile 构造

显示器色彩Profile：从操作系统读取显示器ICC，使用 cmsOpenProfileFromDisplay / 平台WCS接口加载系统显示器配置。
不采用外部icc资源嵌入方案，避免发行包附带独立icc文件、路径丢失问题。

### 预览性能策略
遵循原则：**交互预览与最终导出复用完全同一套管线逻辑、同一参数分支，保证所见即所得，禁止两套独立管线**
1. 交互拖动预览（滑块、CCM矩阵修改、白平衡取点等实时交互）：
   在管线最前端、Demosaic之前，对**拜耳原始浮点数据做Bayer感知下采样**，生成预览分辨率的拜耳数据；
   该低分辨率拜耳送入完全一样的完整渲染管线，所有参数（CCM、demosaic算法、镜头校正、光影、HSL、曲线、降噪锐化全部原样生效）；
   预览分辨率可配置（例如长边1920），由画布窗口大小动态决定。
2. 交互停止（鼠标松开、回车确认参数）：
   使用原始全尺寸拜耳数据，重新跑一遍完整管线，生成全尺寸结果，用于导出/1:1像素检查。
3. 100%像素视图（1:1放大）特例：
   关闭拜耳下采样，取对应区域原始拜耳区块渲染，保证像素级观察；此时不再做低分辨率预览。
4. 约束：
   - 禁止“先全尺寸渲染再缩小RGB”，达不到加速目的；
   - 禁止增量缓存中间RGB图像，减少内存占用与逻辑分支；
   - Bayer下采样实现需要感知拜耳网格布局，不能无视RGGB/GRBG等pattern直接像素平均，避免预览出现伪色、色差偏移；
   - 预览管线和大图管线共用同一个 PipelineRenderer 实现，仅输入拜耳buffer尺寸不同。
