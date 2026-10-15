# Tab5 EPUB 阅读器与 CrossMux 移植可行性评估

> 项目：[doublemarkpro/ESP32-HanDictionary](https://github.com/doublemarkpro/ESP32-HanDictionary)  
> 目标硬件：M5Stack Tab5（ESP32-P4、1280×720 触控屏）  
> 评估日期：2026-10-15  
> 评估源码快照：ESP32-HanDictionary `300e87ce2762c237eae595d398c351d7a270681c`；CrossMux `83b0f810c8628701456e028c2d8566a9be149755`

## 1. 结论

Tab5 可以做成一台体验良好的掌上 EPUB 阅读器，而且现有项目已经具备显示、触控、存储、字体和 UI 等关键基础。

分目标判断：

| 目标 | 可行性 | 判断 |
|---|---:|---|
| 在现有固件内增加实用 EPUB 阅读器 | 8.5/10 | 高度可行，推荐 |
| 移植 CrossMux 的 EPUB、分页和缓存核心 | 7/10 | 可行，是性价比最高的路线 |
| 将完整 CrossMux 固件原样搬到 Tab5 | 4/10 | 技术上可做，但改造量大、收益低 |
| 同时保留现有学习功能和 CrossMux 全部功能 | 3/10 | Flash、框架和维护成本都不理想 |

推荐路线不是把两套完整固件拼接，而是：

> 保留现有 Tab5 BSP、ESP-IDF、LVGL 和儿童学习 UI，选择性移植 CrossMux 的 EPUB 解析、分页、缓存、目录和书签能力。

## 2. 为什么 Tab5 硬件足够

根据 [M5Stack Tab5 官方规格](https://docs.m5stack.com/en/core/Tab5)，主要资源为：

- ESP32-P4，双核 RISC-V，最高 360MHz；
- 16MB Flash；
- 32MB Octal PSRAM；
- 5 英寸 1280×720 IPS 触控屏，MIPI-DSI 接口；
- ESP32-C6 无线模块；
- microSD 卡槽；
- RX8130CE RTC；
- 扬声器、耳机、双麦克风和音频编解码器；
- 可更换 NP-F550 电池。

CrossMux 最初需要在约 380KB 可用 RAM、无 PSRAM 的 ESP32-C3 上工作，并以 800×480 单色墨水屏为基线。与之相比，Tab5 的 CPU、PSRAM和显示带宽都宽裕很多。[CrossMux 硬件约束](https://github.com/0x1abin/crossmux/blob/main/docs/engineering/hardware-constraints.md)

当前 ESP32-HanDictionary 工程已经跑通：

- ST7121/ST7123 显示与触控自动识别；
- MIPI-DSI 显示链路；
- LVGL 9.5；
- PSRAM 分配与图像资源；
- microSD 和 USB 读卡器模式；
- 中文字体和 SD 字体；
- 深色主题；
- 后台工作队列；
- 字典、音频、Wi-Fi、RTC 和电源管理。

因此新增阅读器的主要工作是内容解析、分页与阅读交互，而不是重新解决硬件驱动。

### 2.1 产品定位限制

Tab5 更适合定位为“彩屏掌上阅读学习机”，不适合用墨水屏阅读器的续航标准衡量。

官方标称在亮度 50%、Wi-Fi 常开、后台任务运行的条件下续航约 6 小时。实际阅读模式可以通过降低亮度、关闭非必要网络和暂停语音/摄像头模块来改善，但最终数字必须以真机放电测试为准。

## 3. CrossMux 可以复用的能力

[CrossMux](https://github.com/0x1abin/crossmux) 已经包含较完整的嵌入式阅读系统：

- EPUB、TXT、XTC/XTCH 和图片阅读；
- 书架和文件浏览；
- OPF、TOC 和章节处理；
- HTML、CSS 子集和图片布局；
- 章节导航、书签、脚注和阅读进度；
- 自定义字体；
- 离线字典；
- 阅读统计；
- KOReader 进度同步；
- Calibre、OPDS、WebDAV 和浏览器传书；
- SD 卡页面缓存；
- 触控设备支持和多个 ESP32-S3 硬件目标。

CrossMux 的阅读流水线大致是：

```text
选择图书
   ↓
解析 EPUB 容器、OPF、TOC 和 CSS
   ↓
建立或加载 book.bin / css_rules.cache
   ↓
解析章节 HTML 并执行分页
   ↓
将分页结果写入 SD section cache
   ↓
加载当前 Page 模型
   ↓
通过 GfxRenderer 绘制到显示帧缓冲
```

参考：[CrossMux 架构与阅读流水线](https://github.com/0x1abin/crossmux/blob/main/docs/contributing/architecture.md)。

## 4. 为什么不能直接复制 CrossMux

### 4.1 构建框架不同

CrossMux 使用 PlatformIO 和 Arduino 框架；ESP32-HanDictionary 使用 ESP-IDF。CrossMux 的阅读代码仍可见以下依赖：

- Arduino `Print`；
- `millis()`、`delay()`；
- SdFat、`FsFile`、`HalFile`；
- PlatformIO 库依赖组织；
- Arduino 网络和文件接口。

需要提供 ESP-IDF 兼容层，或者逐步将这些依赖替换为标准 C++、FreeRTOS 和 FatFS/VFS 接口。

### 4.2 显示模型不同

CrossMux 的 `GfxRenderer` 主要面向：

- 1-bit 或有限灰阶帧缓冲；
- 墨水屏全刷、快刷和局刷；
- 刷新波形与残影控制；
- 单帧缓冲、慢刷新设备。

Tab5 使用彩色 MIPI-DSI LCD 和 LVGL，不需要保留墨水屏刷新策略。可以保留分页和文本绘制逻辑，但显示提交层需要重写。

### 4.3 UI 框架不同

CrossMux 使用 Activity、FreeInkUI、UITheme 和物理按键/触控映射；现有项目使用 `HanDisplay` 和 LVGL 页面。

如果完整移植 CrossMux UI，就会在同一固件中维护两套页面、主题、输入和生命周期系统。更合理的方式是重新用 LVGL 实现：

- 书架；
- 阅读页；
- 目录；
- 字号和主题设置；
- 书签列表；
- 查字典弹窗。

### 4.4 P4 无线架构不同

Tab5 的 ESP32-P4 本身没有集成 Wi-Fi，网络由 ESP32-C6 协处理器提供。现有项目已经处理这一硬件组合，但 CrossMux 的普通 ESP32 Wi-Fi 初始化不能原样复用。

## 5. 推荐的软件架构

```text
┌─────────────────────────────────────────────┐
│ 现有 Tab5 BSP / ESP-IDF / LVGL / 音频 / 网络 │
└───────────────────┬─────────────────────────┘
                    │
┌───────────────────▼─────────────────────────┐
│ ReaderApp：书架、阅读页、目录、书签、设置     │
│ 使用现有 LVGL 风格和页面导航                  │
└───────────────────┬─────────────────────────┘
                    │
┌───────────────────▼─────────────────────────┐
│ ReaderEngine                                  │
│ EPUB / OPF / TOC / HTML / CSS / 分页 / 缓存 │
│ 选择性移植自 CrossMux                         │
└──────────┬────────────┬────────────┬─────────┘
           │            │            │
     StorageAdapter  StreamAdapter  FontAdapter
           │            │            │
       FatFS/VFS     ESP-IDF 时间    SD 中文字体
           │
 /sdcard/handict/books 与 reader-cache
```

建议模块：

```text
components/epub_reader/
├── core/                 # EPUB、OPF、TOC、HTML、CSS、分页
├── storage/              # FatFS/VFS 适配
├── render/               # Page 模型和 Tab5 绘制目标
├── cache/                # book、section、cover、progress 缓存
├── fonts/                # SD 字体和字形度量适配
└── compatibility/        # Print、时间、日志等最小兼容层

main/han_dictionary/reader/
├── reader_controller.*   # 生命周期和后台任务
├── library_page.*        # 书架
├── reading_page.*        # 阅读页
├── toc_page.*            # 目录
├── reader_settings.*     # 字号、行距、边距、主题
└── reader_dictionary.*   # 选词并跳转现有字典
```

## 6. 渲染方案

### 6.1 MVP 方案

第一版可以保留 CrossMux 的单色页面模型：

- 1280×720 的 1-bit 帧缓冲约为 115,200 字节；
- 放入 PSRAM；
- CrossMux 的文本绘制逻辑写入该帧；
- 最后转换或映射为 LVGL 可显示的图像；
- 阅读菜单、目录和弹窗仍由 LVGL 彩色绘制。

优点是最容易验证分页正确性和兼容性。

### 6.2 产品方案

产品版可提供新的 `ReaderRendererTarget`：

- 灰阶或 RGB565 页面画布；
- 中文字体抗锯齿；
- 彩色高亮、链接和图片；
- 可利用 ESP32-P4 PPA 加速不透明填充和图像搬运；
- 页面作为单个画布显示，避免为每个词或字符创建大量 LVGL 对象。

不建议把正文拆成几百个 LVGL Label。对象数量、重排成本和堆碎片都会明显增加。

## 7. 与现有字典结合

这是最值得做的差异化功能。

建议交互：

1. 长按正文中的汉字或词语；
2. 阅读器根据分页模型找到当前位置对应的 UTF-8 文本范围；
3. 对单字直接调用现有汉字字典；
4. 对词语先显示短释义，或拆分为单字入口；
5. 返回阅读页时保持原章节、页码和高亮位置。

第一版可以只支持单个汉字，随后再增加分词和词典。

## 8. EPUB 支持范围建议

### 第一版承诺

- 无 DRM 的可重排 EPUB 2/3；
- UTF-8 中文正文；
- 常见段落、标题、粗体、斜体；
- 目录和章节跳转；
- JPEG、PNG 封面和常见正文图片；
- 字号、行距、边距和明暗主题；
- 阅读进度和断电恢复；
- 书签；
- 单字查字典。

### 后续支持

- 脚注和页内链接；
- 搜索；
- 阅读统计；
- 自动翻页；
- Calibre 或浏览器传书；
- OPDS/WebDAV；
- 跨设备进度同步。

### 暂不承诺

- 商业 DRM；
- 固定版式 EPUB；
- MathML；
- SVG 动画；
- EPUB 音视频；
- 浏览器级完整 CSS；
- 极端复杂表格和多栏出版物。

## 9. Flash 与分区风险

这是比 RAM 更重要的硬约束。

当前项目的 `partitions/han16m.csv` 使用两个 `0x600000` 的 OTA 应用槽，即每槽约 6MiB：[当前分区表](https://github.com/doublemarkpro/ESP32-HanDictionary/blob/300e87ce2762c237eae595d398c351d7a270681c/partitions/han16m.csv)。

现有真机记录中的应用约为 4.54MB：[ESP32-HanDictionary 真机记录](https://github.com/doublemarkpro/ESP32-HanDictionary/blob/300e87ce2762c237eae595d398c351d7a270681c/docs/HARDWARE_BRINGUP.md#L7)。

CrossMux 的某个高 DPI 完整构建记录约为 6.38MB：[CrossMux 高 DPI 体积记录](https://github.com/0x1abin/crossmux/blob/83b0f810c8628701456e028c2d8566a9be149755/docs/engineering/high-dpi-ui-profile.md#L376)。这个数据不是 Tab5 最终构建体积，但足以说明完整 CrossMux 已接近或超过现有应用槽的量级。

因此：

- 不能把两套完整固件简单合并；
- 每个里程碑都必须运行 `tools/capacity_report.py`；
- 阅读器大型字体、封面、CSS 缓存和连字符词典应放到 microSD；
- 应裁掉墨水屏驱动、游戏、重复网络服务和不需要的语言资源；
- 不建议为了完整 CrossMux 立即放弃 A/B OTA；
- 如果精简后仍然超过安全线，再单独评估分区调整。

## 10. 内存与任务建议

- EPUB 解析和章节分页放在后台 FreeRTOS 任务；
- LVGL 线程只接收不可变结果并更新画面；
- 页模型、图片解码和字体缓存优先放 PSRAM；
- SD 文件访问统一序列化，避免与字典、天气、音频和 USB MSC 冲突；
- 切换为 USB 读卡器模式前必须关闭图书和缓存文件；
- 进入阅读器时记录内部 RAM、PSRAM 总空闲量和最大连续块；
- 阅读进度不要每翻一页都同步写入，使用定时或页数去抖；
- 图片、章节和缓存失败必须允许降级，不能导致整个图书退出。

## 11. 开发里程碑与工期

以下为一名熟悉 ESP-IDF/C++ 的开发者估算，不包含大规模硬件生产验证。

### 阶段 0：技术验证，3–7 天

- 在 ESP-IDF 中编译最小 EPUB/ZIP/XML 代码；
- 从 `/sdcard/handict/books/` 打开一本 EPUB；
- 读取标题、作者、封面和目录；
- 分页并绘制一页中文正文；
- 支持前后翻页；
- 测量固件增量、内部 RAM、PSRAM 和分页耗时。

通过标准：

- 3 本结构不同的中文 EPUB 均能解析；
- 不阻塞 LVGL 和语音主任务；
- 重复打开时能利用缓存；
- 固件仍保留安全 OTA 余量。

### 阶段 1：MVP，3–5 周

- 书架和封面；
- 目录；
- 正文阅读；
- 字号、行距、边距；
- 明暗主题；
- 阅读进度和断电恢复；
- 基础错误提示；
- 长按汉字查现有字典。

### 阶段 2：实用版本，再 3–6 周

- 正文图片；
- 书签；
- 脚注和链接；
- 缓存管理；
- 图书导入；
- 更完整的 CSS；
- 自动翻页；
- 阅读统计；
- 异常 EPUB 兼容测试。

### 阶段 3：接近完整 CrossMux，累计 3–6 人月

- 浏览器传书；
- Calibre；
- OPDS/WebDAV；
- 网络 OTA；
- KOReader 同步；
- 多语言；
- 完整字体管理；
- 大规模 EPUB 测试集和长时间稳定性测试。

## 12. 建议的首个验收目标

不要一开始追求完整 CrossMux。建议首个项目目标定义为：

> 20 本不同来源的无 DRM 中文儿童 EPUB 能稳定打开、显示封面和目录、连续翻页、调整字号、断电恢复，并能在阅读页长按汉字进入现有字典。

建议测试图书覆盖：

- EPUB 2 与 EPUB 3；
- 有封面和无封面；
- 大章节和小章节；
- 含 JPEG/PNG 图片；
- 内嵌 CSS 和无 CSS；
- 简体中文、繁体中文、英文混排；
- 异常或缺失元数据；
- 大于 50MB 的图书；
- SD 热拔插和空间不足；
- 阅读中进入闹钟、语音或锁屏后恢复。

## 13. 不推荐的做法

- 不要直接复制 CrossMux 的整个 `src/` 和 `freeink-sdk/`；
- 不要在现有 `HanDisplay` 中继续堆入几千行阅读器解析代码；
- 不要为正文中的每个字或词创建 LVGL 对象；
- 不要把完整 CJK 字体和全部图书资源链接进固件；
- 不要在 LVGL 线程同步解析整章；
- 不要为了第一次验证就修改 OTA 分区；
- 不要把成功编译当作真机兼容性验证。

## 14. 开源许可证

ESP32-HanDictionary 与 CrossMux 均采用 MIT License，代码复用在许可证层面兼容。

移植时需要：

- 保留 CrossMux 及其上游代码的版权声明；
- 在项目 LICENSE/NOTICE 或第三方依赖文档中注明来源；
- 继续分别检查字体、图片、声音、词典和测试图书的授权；
- 不要把商业图书或未授权词典提交到公开仓库。

## 15. UI 概念图

以下图片是方向稿，不是已在真机运行的界面。最终应使用 LVGL 原生实现，而不是直接把图片作为整页贴图。

### 15.1 主页增加“阅读”入口

![Tab5 阅读器主页概念图](./tab5-reader-home-concept.png)

### 15.2 儿童书架

![Tab5 EPUB 书架概念图](./tab5-reader-library-concept.png)

### 15.3 阅读与查字典

![Tab5 EPUB 阅读页概念图](./tab5-reader-page-concept.png)

## 16. 效果图生成说明

概念图使用内置 ImageGen 生成，并以项目现有主页和字典截图作为视觉参考。

最终提示词组的核心要求为：

1. 保持现有奶油色背景、深蓝圆体标题、彩色圆角卡片和儿童插画语言，在主页加入“阅读”入口；
2. 创建带封面、阅读进度、搜索、筛选和 SD 导入入口的儿童书架；
3. 创建安静、清晰的横屏阅读页，展示目录、字体、书签、翻页、阅读进度和选词查字典交互。

