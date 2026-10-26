# Tab5 EPUB 阅读模块开发记录

更新日期：2026-10-15

本文是阅读模块的开发入口，同时记录已完成范围、TF 卡目录、验证命令和后续阶段。目标是在保留 HanDictionary 的 Tab5 BSP、ESP-IDF 6、LVGL、语音和 USB MSC 能力的前提下，用阅读模块替换主页原有闹钟入口。

## 当前完成范围（阶段 1）

- 主页第六个入口已由“闹钟”改为“阅读”，阅读页接入既有导航和异步工作队列。
- 可扫描 `handict/books/*.epub`，读取 ZIP 中央目录并使用 ESP32 ROM `tinfl` 解压。
- 按 EPUB 标准链路解析 `META-INF/container.xml`、OPF 元数据、manifest 和 spine。
- 可读取 spine XHTML，过滤标签、脚本和样式后显示正文，支持章内分页和跨章节前后翻页。
- 可扫描 `handict/fonts/*.ttf`，在阅读页动态切换 TF 卡字体；加载失败会回退到固件字体。
- 字号、行距、页边距、字体和日/夜模式可在阅读中修改，并保存到 NVS `han_reader` 命名空间。
- EPUB 文件在每次解析/读取后立即关闭，避免阻塞 USB MSC 卸载 TF 卡。
- 对 ZIP 条目数、XML/XHTML 解压后大小、加密条目、压缩算法、边界和 CRC 做了限制或校验。

这是一版可运行的阅读 MVP，不应等同于完整排版引擎。当前只显示抽取后的纯文本；CSS、图片、目录页、脚注、精确字形测量分页、阅读位置持久化和翻页动画仍在后续阶段。

## TF 卡目录

将仓库中的 `content/sdcard/handict` 整体复制到 FAT32 TF 卡根目录。阅读相关目录如下：

```text
handict/
├── books/
│   └── *.epub
├── fonts/
│   └── *.ttf
└── reader-cache/        # 后续阶段使用，当前不会创建
```

字体必须有合法的个人使用或分发授权。大字体可能增加打开时间和 PSRAM 占用，建议先用包含所读语言字形的子集字体测试。EPUB 目前支持普通 ZIP 的 Store（0）和 Deflate（8）条目，不支持加密书籍、DRM 和 ZIP64。

### 大体积中文字体的翻页性能

Tab5 的 FreeType 阅读路径从 TF 卡流式读取字体，并启用 FATFS fast seek：打开只读文件时建立簇链索引，避免随机读取大字体时反复访问 FAT。阅读字体共享 1 MiB、1 KiB 块的四路组相联文件缓存；较小块可减少离散汉字轮廓的读放大。字形度量缓存为 8192 项，位图缓存根据字号限制在 256–2048 项（按约 2 MiB 预算估算）。缓存随 FreeType face 释放，切换字体或 USB MSC 时不会保留文件句柄。

调试时同时观察 `Page 5: build=... refresh=... queued=...` 和 `Reader font I/O: ... SD blocks, ... RAM hits, ... ms SD`。分别测量首次打开、连续新页、返回已读页、字号切换和横竖屏切换；不能用已读页的缓存命中速度代替连续新页速度。该适配器使用当前 LVGL 9 的 FreeType face/cache 结构，升级 LVGL 时需复核该接口与流关闭顺序。

2026-10-26，COM7 Tab5、8 MHz SDSPI、30 px `LXGWWenKaiGBScreen.ttf`，同一本中文 EPUB 的 spine 7 连续第 6–13 页实测总耗时为 1388、1116、802、574、632、401、358、450 ms；对应字体读卡耗时为 831、520、371、250、323、191、152、206 ms。第 13 页显示菜单为 379 ms。该数据覆盖连续新页；首次进入书架时字体创建、扫描和封面解码仍可能产生数秒加载，不能据此宣称所有页面操作都低于一秒。

## 代码结构

- `main/han_dictionary/reader/epub_reader.{h,cc}`：书库/字体扫描、ZIP 条目读取、EPUB package/spine 解析、XHTML 纯文本化。
- `main/han_dictionary/han_display.{h,cc}`：书架、阅读页面、分页、字体生命周期、阅读设置和工作队列。
- `main/CMakeLists.txt`：固件构建接入。
- `tools/ui-simulator/CMakeLists.txt`：桌面 UI 模拟器接入（使用模拟书籍，不访问真实 EPUB）。

实现参考了 CrossMux 的分层和资源生命周期思路，尤其是“小对象保存元数据、章节按需读取、ZIP/SD 句柄及时释放、排版设置与缓存键绑定”。本阶段没有直接复制 CrossMux 的 Arduino/电子纸 UI 栈，也没有照搬其完整阅读器代码。参考基线和取舍见 [移植评估](Tab5-EPUB-CrossMux-porting-assessment.md)。

## 分步开发路线

### 第 0 步：建立可回退基线（已完成）

1. 将 HanDictionary 源码导入本仓库。
2. 单独提交原始基线，确保后续阅读改动可清晰审查或回退。
3. 保留原有 Tab5 板级、SD、LVGL、语音和 USB MSC 实现。

### 第 1 步：阅读 MVP（已完成，待真机验收）

1. 替换主页闹钟入口，建立书架和阅读页。
2. 扫描 EPUB/TTF，解析 container、OPF、manifest、spine。
3. 按章解压 XHTML，并生成适合当前屏幕的近似分页。
4. 增加字号、行距、边距、字体、日夜模式控件和 NVS 设置。
5. 完成 ESP-IDF 固件构建、静态检查和主机测试。

真机验收项目：不同压缩方式 EPUB、中文/英文混排、缺字回退、连续翻页、反复换字体、插拔/USB MSC 切换、峰值 PSRAM 和长时间稳定性。

### 第 2 步：可靠分页与阅读进度

1. 用 LVGL 实际 glyph 宽度替换字符数估算，建立 UTF-8 安全的行布局器。
2. 解析段落、标题、列表、粗斜体和基础 CSS；明确不支持属性时的降级规则。
3. 解析 EPUB 3 nav / EPUB 2 NCX，增加章节目录。
4. 将位置保存为“书籍指纹 + spine 索引 + 章内文本偏移”，避免字号变化后页码失效。
5. 在 `reader-cache` 写入版本化缓存；缓存键必须包含文件大小/修改时间、字体、字号、行距、边距和排版器版本。
6. 增加合成 EPUB 测试样本，覆盖相对路径、XML 命名空间、实体、损坏 ZIP、超大条目和 CRC 错误。

### 第 3 步：TF 字体管理

1. 读取 TTF name 表，显示字体家族名而不是文件名。
2. 启动时只索引元数据，进入阅读或切换字体时才创建 LVGL 字体对象。
3. 缓存单个当前字体，离开阅读/切换 USB MSC 前销毁；记录创建失败原因和最大连续内存。
4. 增加“系统字体”选项、缺字提示和 fallback 链；验证简中、繁中、拉丁和标点。
5. 字体变化后以文本偏移恢复位置并重新分页。

### 第 4 步：翻页动效（涟漪）

1. 先实现 150–220 ms 的滑动/淡入动效，并提供“关闭动效”。
2. 将当前页与下一页渲染到两个离屏 buffer，确认内存预算和 PPA/DMA 对齐。
3. 实现以触点或屏幕边缘为圆心的径向遮罩（ripple reveal），只让遮罩变化区域参与重绘。
4. 动画期间冻结重复翻页输入；章节加载慢时先显示稳定的加载状态。
5. 在 30/40/48 px、多种 TTF 和日夜模式下测量帧率、峰值内存及撕裂；内存不足自动降级到滑动或无动效。

涟漪只属于表现层，必须在精确分页和双页缓存稳定后接入，否则会把 I/O、布局和动画故障混在一起。

### 第 5 步：完整阅读体验

1. 书签、目录、阅读进度条、最近阅读和封面缩略图。
2. 图片按需解码与尺寸限制，脚注/内部链接跳转。
3. 长按选词接入已有字典，阅读位置不因打开词典而丢失。
4. 可选屏幕方向、段落缩进、对齐方式、主题和自动翻页。

### 第 6 步：清理与发布

1. 真机稳定后删除已不可达的旧闹钟 UI/逻辑/资源及 NVS 迁移代码。
2. 至少用 20 本来源合法、结构不同的 EPUB 做兼容性矩阵。
3. 运行容量门禁、全量主机测试、Tab5 构建和真机回归。
4. 更新本文件的“当前完成范围”和限制，打版本标签并发布固件包。

## 构建与验证

在 ESP-IDF 6.0.2 PowerShell 环境中：

```powershell
python scripts/build.py m5stack/tab5 `
  --name m5stack-tab5-han-dictionary `
  --language zh-CN `
  --wake-word wn9l_nihaoxiaozhi_tts3

python tools/capacity_report.py
python -m unittest discover -s scripts/tests -v

$env:HAN_UI_READER_SMOKE = "1"
.\build\ui-simulator-mingw\han_ui_sim.exe .\build\ui-reader-smoke
Remove-Item Env:HAN_UI_READER_SMOKE
```

准备 TF 卡模板：

```powershell
python tools/content_pack.py prepare --output dist/tabreader-card
```

## 换电脑继续开发

```powershell
git clone https://github.com/doublemarkpro/ESP32-TabReader.git
cd ESP32-TabReader
git pull --ff-only origin main
git log --oneline -10
```

然后安装 ESP-IDF 6.0.2，执行上面的构建命令。`build/`、`managed_components/` 和 `dist/` 都是本机生成物，不依赖从另一台电脑复制。书籍和第三方字体通常不提交 Git；将它们放到本机 TF 卡或忽略目录中。

每个开发阶段建议保持以下节奏：

```powershell
git status --short
git diff --check
git add <本阶段文件>
git commit -m "feat(reader): ..."
git push origin main
```

不要把“能编译”写成“真机完成”。每次提交都应在本文记录：完成范围、验证命令、真机结果、已知限制和下一步。

## 2026-10-15 验证记录

- `git diff --check`：通过。
- Python 主机测试：91/91 通过。
- 原生 LVGL 模拟器：MinGW 构建通过；阅读专项 smoke 通过书架、章节、字号重排、日夜切换和返回书架；gallery smoke 通过全部页面渲染。
- ESP-IDF 6.0.2 / `m5stack/tab5` 完整构建、链接和 `merge-bin`：通过。`xiaozhi.bin`
  为 5,066,704 字节（`0x4d4fd0`），6 MiB 应用分区剩余 1,224,752 字节
  （`0x12b030`，19%）；合并镜像为 `build/merged-binary.bin`。
- 真机：尚未执行，因此 TF 卡 TTF 的内存表现、EPUB 兼容性和触摸体验仍标为待验收。

## 已知技术债

- 旧闹钟源码暂时保留，但入口已被替换且启动时不再启用闹钟；真机确认阅读稳定后再删除，便于当前阶段回退和缩小审查范围。
- XML 解析器是受限的轻量实现，尚未覆盖所有 namespace、CDATA 和异常编码组合。
- 当前分页按字号估算字符容量，不是字形级排版；设置变化会回到本章第一页。
- 当前只处理正文文本，不显示 CSS、图片、Ruby、数学公式、脚注和 DRM 内容。
- 涟漪翻页尚未实现；路线中已明确依赖双页缓存、PPA/内存测量和降级路径。
