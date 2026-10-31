// ============================================================================
//  CullWindow.h —— 快速选图工作台
//
//  对应 requirements/view.md 的 MVP：
//   - §一 源 / 目标目录分拣：扫描 RAW + JPG、缩略图网格、复制（不移动）、
//     颜色标记（标记颜色对应一个目标目录）、1~5 星、批量复制、写权限校验、
//     分拣日志、格式过滤
//   - §二 固定视野构图：居中固定比例框、框外压暗（40%~100%）、1px 固定框线、
//     构图线、图像拖动缩放、仅预览旋转、全屏预览
//   - §三 MVP：星级与标记筛选、按文件名 / 拍摄时间 / 文件大小排序、
//     仅预览旋转、全屏预览
//
//  设计约束（需求 §五）：
//   - 不侵入 RAW 解码调色管线：预览复用 RawLoader + PipelineRenderer，
//     但使用独立的渲染器实例与独立线程，不与界面线程争抢
//   - 星级 / 标记只写源目录下的 .rawedit.pick.json，绝不修改 RAW / EXIF
//   - 构图框是纯视觉叠加层，不参与色彩管线，也不生成裁切文件
//   - 复制是操作系统层面的文件复制，原文件保留
// ============================================================================

#pragma once

#include "io/ImageMetadata.h"
#include "io/PickStore.h"

#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QKeySequence>
#include <QMainWindow>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <memory>

class QCheckBox;
class QComboBox;
class QDir;
class QButtonGroup;
class QDoubleSpinBox;
class QFileSystemWatcher;
class QGridLayout;
class QHBoxLayout;
class QKeyEvent;
class QShortcut;
class QShowEvent;
class QTimer;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSlider;
class QSplitter;
class QTextEdit;
class QThread;
class QToolButton;
class QVBoxLayout;
class QWidget;

namespace rawui {

class CullPreviewWidget;
class CullThumbnailWorker;
class CullPreviewWorker;
class IccSimulatorWindow;
class HelpDialog;
/// 扁平五角星评分控件（定义在 CullWindow.cpp，仅本工作台使用）
class StarRatingWidget;
/// 点击后「按新键即录入」的快捷键按钮（定义在 CullWindow.cpp）
class ShortcutKeyButton;

class CullWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit CullWindow(QWidget* parent = nullptr);
    ~CullWindow() override;

    /// 打开目录（也可以传入目录中的某个文件，取它所在的目录并选中该文件）。
    /// 启动参数传入文件 / 目录时使用。
    void openDirectory(const QString& directoryOrFile);

signals:
    // 发往工作线程的请求。与工作对象的槽是队列连接，因此不会在 UI 线程执行。
    void metadataRequested(const QString& path);
    void thumbnailRequested(const QString& path, int maxSize);
    void previewRequested(const QString& path, int maxDimension, quint64 ticket);
    void usePipelineThumbnailsChanged(bool enabled);
    /// 镜头畸变校正开关变化（发给缩略图 / 预览两个工作线程）
    void lensCorrectionChanged(bool enabled);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    /// 目录中的一张图片及其挑片状态
    struct Item {
        QString path;          ///< 绝对路径
        QString relativePath;  ///< 相对源目录，用作侧车 JSON 的键
        rawio::ImageFileInfo info;
        bool metadataLoaded = false;
        int star = 0;
        QString colorLabel;            ///< 标记颜色（Red / Yellow / …），见下方目标目录
        /// 已处理：这张图复制到目标目录后留下的副本完整路径（可能有多份）。
        /// 非空即视为已处理；改变 / 清除标记时要按这些路径删除旧副本。
        QStringList copiedDestinations;
        QImage thumbnail;              ///< 未装饰的缩略图
        /// 扫描时的最后修改时间，与文件大小一起用于发现文件被外部覆盖
        QDateTime lastModified;
        QListWidgetItem* listItem = nullptr;  ///< 当前列表中的行（被筛掉时为 null）
    };

    /// 一个分拣目标目录：路径 + 该目录对应的标记颜色。
    /// 颜色名同时写进侧车 label，批量复制靠它把图片分发到对应目录。
    struct TargetDir {
        QString path;
        QString color;      ///< 颜色名（Red / Yellow / Green / …），作为标记的稳定标识
        QString customHex;  ///< 自定义颜色（#rrggbb）；为空表示使用预设色
    };

    // ---- 界面搭建 ----
    void buildUi();
    QWidget* buildLeftPanel();
    QWidget* buildPreviewPanel();
    QWidget* buildThumbnailPanel();
    /// 构建设置浮层：缩略图管线、镜头畸变校正与快捷键表
    QWidget* buildSettingsPanel(QWidget* parent);
    void connectSignals();

    // ---- 设置持久化 ----
    void loadSettings();
    void saveSettings();

    // ---- 构图辅助 ----
    void applyCompositionToPreview();
    void applyAspectFromUi();
    /// 固定视野关闭时，比例 / 框线 / 压暗等细化参数不可用。
    /// 保证"先开启固定视野，再选比例"的操作顺序。
    void updateCompositionControls();
    void setCompositionEnabled(bool enabled);
    void chooseFrameColor();
    void setFullScreenMode(bool enabled);
    /// 显示 / 收起浮在画面上的「构图与视图」面板，并同步标记行按钮的选中态
    void setCompositionPanelVisible(bool visible);
    /// 把浮层面板贴到预览控件左上角（预览尺寸变化时重新贴边）
    void positionCompositionPanel();
    /// 显示 / 收起浮在画面上的「设置」面板（与构图浮层互斥，不会叠在一起）
    void setSettingsPanelVisible(bool visible);
    void positionSettingsPanel();
    /// 打开 / 关闭悬浮的 ICC 模拟器窗口（按钮的选中态即窗口可见性）
    void toggleIccSimulator();
    /// 弹出「帮助」窗口（功能介绍、工作流示例、快捷键与联系方式），重复点击只置前
    void openHelp();

    // ---- 快捷键 ----
    /// 把 m_shortcuts 填成内置默认键（不读写设置）
    void initDefaultShortcuts();
    void loadShortcuts();
    void saveShortcuts();
    /// 按 m_shortcuts 刷新每颗按键按钮的文字与「复制并下一张」的 QShortcut
    void refreshShortcutButtons();
    /// 同步控件提示里的键位文字（改键 / 清除 / 全屏切换后调用）
    void refreshShortcutHints();
    void applyCopyShortcut();
    /// 给某个动作绑定按键；空序列表示清除（该动作不再响应键盘）
    void assignShortcut(const QString& id, const QKeySequence& sequence);
    /// 全部动作恢复默认键
    void resetAllShortcuts();
    /// 动作当前绑定的按键；已被清除时返回空序列
    QKeySequence shortcutSequence(const QString& id) const;
    /// 这个按键事件是否命中某个动作（修饰键也参与比较）
    bool matchesShortcut(const QString& id, const QKeyEvent* event) const;
    /// 执行动作（键盘、QShortcut 共用的唯一入口）
    void triggerShortcutAction(const QString& id);

    // ---- 目录与列表 ----
    void scanDirectory();
    /// 根据磁盘文件构造一个列表条目（整目录扫描与新增构图视野文件共用）
    Item makeItem(const QString& path, const QDir& rootDir) const;
    /// 列表中的某张图片被外部程序覆盖后，刷新它的元数据、缩略图与预览。
    /// 只更新这一条，不重建整个网格。
    void refreshItemFromDisk(const QString& path);
    void rebuildList();
    /// 时间分组标题行宽度：跟随缩略图可视宽度，保证独占整整一行
    int groupHeaderWidth() const;
    /// 列表 / 面板尺寸变化后刷新所有分组标题行的宽度
    void updateGroupHeaderWidths();
    /// 同步「时间分组」相关控件的可用状态
    void updateTimeGroupControls();
    /// 折叠 / 展开一个时间分组：隐藏组内缩略图，并记住状态供列表重建后恢复
    void toggleTimeGroupCollapsed(QListWidgetItem* header);
    void queueFileJobs();
    void refreshItemText(Item& item);
    void refreshItemIcon(Item& item);
    void updateInfoBar();
    void updateActionState();
    /// 刷新状态栏右下角的"已选中 / 总数"计数
    void updateSelectionInfo();
    void showStatus(const QString& text);
    /// 重建「显示」筛选下拉框：星级用 ★ 个数表示，已标记颜色带圆点和名称。
    /// 保留当前选中的筛选项；对应颜色被删掉时回落到「全部图片」。
    void rebuildFilterCombo();
    /// 颜色标识对应的圆点图标（筛选下拉框用）
    QIcon colorDotIcon(const QString& color) const;
    /// 当前筛选项的稳定键：all / star1…star5 / marked / unmarked / color:<颜色名>。
    /// 用键而不是下标，目标目录增删后选项位置变化也不会串掉。
    QString currentFilterKey() const;
    void setFilterKey(const QString& key);
    /// 按星级刷新右下角的五颗星（空心 / 金色填充）
    void updateStarRating(int star);
    /// 按当前图片的标记颜色刷新颜色圆点的选中状态
    void updateColorLabelSelector(const QString& label);

    // ---- 目标目录（每个目录带一个标记颜色）----
    /// 重建左侧目标目录行（路径 + 颜色圆点 + 修改 / 删除按钮）
    void rebuildTargetDirRows();
    /// 重建预览下方的颜色圆点：一个目标目录一个圆点
    void rebuildColorChips();
    void addTargetDirectory();
    void changeTargetDirectory(int index);
    void removeTargetDirectory(int index);
    /// 点击目标目录的颜色圆点：用取色器自定义这个目录的标记颜色
    void customizeTargetColor(int index);
    /// 颜色名对应的目录下标；没有对应目录时返回 -1
    int targetIndexForColor(const QString& color) const;
    /// 图片应当复制到哪个目录：优先按标记颜色匹配；
    /// 没有标记时只在"仅有一个目标目录"的情况下回落，避免误投。
    QString targetDirectoryForItem(const Item& item) const;
    /// 颜色表中第一个尚未被占用的颜色；颜色表用尽时返回空串
    QString nextFreeTargetColor() const;
    /// 把某个颜色下的所有图片标记恢复为未标记（目录清除时用），返回清理数量
    int clearColorMarks(const QString& color);
    /// 颜色标识对应的显示色：目录自定义过就返回自定义色，否则取预设色
    QString targetColorHex(const QString& color) const;
    QString targetColorDisplayName(const QString& color) const;

    void selectOffset(int delta);
    /// 在缩略图网格里按行上下移动（同一列优先，末行缺列时取最近的一个）
    void selectVertical(int direction);
    /// 按当前行位置刷新标记组两侧箭头的可用状态（首张 / 末张自动置灰）
    void updateNavButtons();
    void selectPath(const QString& path);
    Item* itemForPath(const QString& path);
    Item* currentItem();
    /// 把列表当前行同步成"当前图片"（信息栏 / 动作按钮 / 预览）
    void syncCurrentItem();

    // ---- 挑片动作 ----
    void setCurrentStar(int star);
    void copyCurrent(bool advance);
    /// 把所有带颜色标记的图片按颜色分发到对应目标目录
    void copyMarked();

    // ---- 空格快速标记 / 复制 ----
    /// Space 按下：未标记的图片直接弹出浮动色板；
    /// 已标记的图片先等待松手（短按复制并下一张），按住满 1 秒再弹出色板改标记。
    void handleSpacePressed();
    /// Space 在等待期内松开：执行复制并下一张
    void handleSpaceReleased();
    /// Space 按住满 1 秒：弹出色板重新选择标记颜色
    void onSpaceHoldTimeout();
    /// 放弃本次 Space 动作（不复制）
    void cancelSpacePress();
    /// 等待期与色板期共用 qApp 事件过滤器，统一管理避免重复安装
    void installSpaceEventFilter();
    void uninstallSpaceEventFilter();
    void beginSpaceColorPicker();
    /// 按当前目标目录重建色板圆点（最左边是「不选中」）
    void rebuildSpaceColorPicker();
    void updateSpaceColorPickerSelection();
    void moveSpaceColorPickerSelection(int delta);
    /// commit 为 true 时应用选中的颜色并复制 & 下一张；
    /// 最左边「不选中」则清除标记（已复制过的副本一并删除），不复制
    void finishSpaceColorPicker(bool commit);

    // ---- F10：选图工作台进阶 ----
    void setCurrentColorLabel(const QString& label);
    void exportCsv();
    void renameSelection();
    /// 在源目录里为选中的图片创建文件副本（文件名追加「 - 副本」）
    void createSelectionCopies();
    /// 创建副本时使用的目标路径：同名副本已存在时追加 (2)、(3)…
    QString duplicateDestination(const QString& sourcePath) const;
    /// 截取当前取景框内的画面，直接保存到原图所在目录
    void captureCompositionSample();
    /// 复制单张到指定目录，返回 false 表示失败，原因写入 error。
    /// alreadyExists 为 true 表示目标目录里已有内容完全相同的副本，本次跳过复制。
    bool copyOne(Item& item, const QString& targetDir, bool& alreadyExists, QString& error);
    /// 删除这张图此前复制到目标目录的副本（改变 / 清除标记时调用）。
    /// keepDirectory 非空时保留该目录下的副本（新标记仍指向同一目录，副本继续有效）。
    /// 同时写一条「删除」日志，重启后不会再把它算作已处理。
    void removeCopiedCopies(Item& item, const QString& keepDirectory = QString());
    /// 为目标文件挑选落点：目标目录已有内容完全相同的副本时返回该路径并置
    /// alreadyExists；同名但内容不同的文件不算同一张图，继续尝试 _1、_2…
    QString uniqueDestination(const QString& targetDir, const QString& sourcePath,
                              const QString& fileName, bool& alreadyExists) const;
    bool ensureTargetDirectory(const QString& dir, QString& error);

    // ---- 预览 ----
    void requestPreviewForCurrent(bool force);
    /// preserveView 为 true 时，新像素替换旧像素但不重置缩放 / 平移。
    /// 切换渲染选项（例如畸变校正开关）时使用：用户正在看的位置不能跳。
    void applyPreviewImage(const QString& path, const QImage& image,
                           bool preserveView = false);
    /// 放大到预览图不再是 1:1 时，按需渲染原图分辨率的细节图。
    /// 3072 长边的预览在大屏上放大后会变糊，且 2x2 合并的色度无法
    /// 完全避免拍频彩纹；原图分辨率走全分辨率去马赛克，两者都会改善。
    void maybeRequestDetail();
    /// 按当前图片是否带内嵌 JPEG，同步「内嵌 JPEG 预览」按钮的可用状态
    void updateEmbeddedJpegButton();

    // ---- 日志 ----
    void appendLogLine(const QString& line);
    /// 单条分拣日志：颜色名前面补同色圆点
    void appendLogEntry(const rawio::PickLogEntry& entry);
    /// 追加一段富文本（调用方负责转义其中的用户文本）
    void appendLogHtml(const QString& html);
    void reloadLogView();

    // ---- 工作线程回调 ----
    void onWorkerMetadata(const QString& path, const rawio::ImageFileInfo& info);
    void onWorkerThumbnail(const QString& path, const QImage& image);
    void onWorkerThumbnailFailed(const QString& path, const QString& reason);
    void onPreviewReady(const QString& path, const QImage& image, quint64 ticket);
    void onPreviewFailed(const QString& path, const QString& message, quint64 ticket);

    /// 处理快捷键；返回 true 表示已消费
    bool handleKeyPress(QKeyEvent* event);

    // ---- 数据 ----
    QVector<Item> m_items;
    QHash<QString, int> m_indexByPath;
    QString m_currentPath;
    std::unique_ptr<rawio::PickStore> m_store;
    /// 缩略图尺寸与大图预览长边。
    /// 预览长边提高到 3072：挑片时放大到 100% 附近仍然清晰，
    /// 配合 CullPreviewWidget 的 mipmap 采样，也让"适合窗口"不再出摩尔纹。
    static constexpr int kThumbnailSize = 240;
    static constexpr int kPreviewMaxDimension = 3072;
    /// 1:1 细节渲染的长边请求值。只要不小于原始长边，管线就关闭拜耳域
    /// 降采样、按原图分辨率去马赛克（需求 append_v3 §3「1:1 原始像素视图」）。
    /// 取值只作为"足够大"的阈值，不会真的放大图像。
    static constexpr int kDetailMaxDimension = 20000;
    /// 已渲染预览的缓存条目数（来回翻看同一批图时不必重复解码）
    static constexpr int kPreviewCacheEntries = 4;
    QCache<QString, QImage> m_previewCache{kPreviewCacheEntries};
    quint64 m_previewTicket = 0;
    /// 1:1 细节状态。细节图不进 m_previewCache：单张动辄上百 MB，
    /// 缓存四张会挤占内存；只在当前图片上保留一份。
    QString m_detailPath;
    quint64 m_detailTicket = 0;
    /// 当前这张图是否已经拿到 3072 快速预览（细节请求要等它先到）
    QString m_previewReadyPath;
    /// 等待中的预览到达后要保留视图（记录目标图片路径）。
    /// 只切换渲染选项、不换图时才设置；换图后路径不匹配，自然失效。
    QString m_preserveViewPath;
    /// 预览是否使用相机内嵌 JPEG（默认开启，显示相机直出效果）
    bool m_useEmbeddedJpeg = true;
    int m_pendingMetadata = 0;
    bool m_fullScreen = false;
    bool m_rebuilding = false;

    // ---- 工作线程 ----
    QThread* m_thumbThread = nullptr;
    QThread* m_previewThread = nullptr;
    CullThumbnailWorker* m_thumbWorker = nullptr;
    CullPreviewWorker* m_previewWorker = nullptr;

    // ---- 界面 ----
    QWidget* m_leftPanel = nullptr;
    QWidget* m_thumbnailPanel = nullptr;
    QLineEdit* m_sourceEdit = nullptr;
    /// 目标目录列表与左侧动态行容器（每个目录一个颜色圆点）
    QVector<TargetDir> m_targetDirs;
    QGridLayout* m_dirGrid = nullptr;
    QVBoxLayout* m_targetRowsLayout = nullptr;
    QWidget* m_targetRowsHost = nullptr;
    /// 「＋ 添加目标目录」按钮：没有目录行时与「目标目录」标题同一行，
    /// 有目录行时排在所有行的下方（由 rebuildTargetDirRows() 摆位）
    QPushButton* m_addTargetButton = nullptr;
    /// 预览下方由目标目录动态生成的颜色圆点
    QHBoxLayout* m_colorChipsLayout = nullptr;
    /// 按住空格时叠在预览上的浮动色板（第一个圆点是「不选中」）
    QWidget* m_colorPickerPopup = nullptr;
    QHBoxLayout* m_colorPickerLayout = nullptr;
    QVector<QToolButton*> m_colorPickerButtons;
    int m_colorPickerIndex = 0;
    bool m_spacePickerActive = false;
    /// Space 已按下但还没松手（已标记图片的短按 / 长按判定）
    bool m_spacePressActive = false;
    /// 按住多久弹出色板（已标记图片）
    QTimer* m_spaceHoldTimer = nullptr;
    /// qApp 事件过滤器是否已安装（等待期与色板期共用）
    bool m_spaceFilterInstalled = false;
    QCheckBox* m_recursiveCheck = nullptr;
    QComboBox* m_formatCombo = nullptr;
    QComboBox* m_filterCombo = nullptr;
    QComboBox* m_sortCombo = nullptr;
    QButtonGroup* m_colorGroup = nullptr;
    QFileSystemWatcher* m_watcher = nullptr;
    QTimer* m_refreshTimer = nullptr;
    QCheckBox* m_descendingCheck = nullptr;
    /// 时间分组：勾选后按拍摄时间排序并插入分组标题
    QCheckBox* m_timeGroupCheck = nullptr;
    /// 分组的相邻时间间隔阈值（下拉项 data 为分钟数）
    QComboBox* m_timeGroupGapCombo = nullptr;
    /// 已折叠的时间分组（按标题记录）：列表重建后恢复折叠状态
    QSet<QString> m_collapsedTimeGroups;
    QCheckBox* m_pipelineThumbCheck = nullptr;
    QListWidget* m_list = nullptr;
    /// 窗口第一次被激活时，无论 Qt 把默认焦点给了哪个控件，
    /// 都统一改交给缩略图列表（后续激活不强行打断用户当前焦点）
    bool m_initialFocusApplied = false;
    QTextEdit* m_logView = nullptr;

    QLabel* m_infoLabel = nullptr;
    /// 状态栏右下角常驻的"已选中 / 总数"计数
    QLabel* m_selectionLabel = nullptr;
    CullPreviewWidget* m_preview = nullptr;

    QCheckBox* m_compositionCheck = nullptr;
    /// 预览镜头畸变校正（按 EXIF 自动匹配镜头库）
    QCheckBox* m_lensCorrectionCheck = nullptr;
    QComboBox* m_aspectCombo = nullptr;
    /// 自定义比例的「宽 ∶ 高」输入组，选中「自定义」时才显示
    QWidget* m_customAspectBox = nullptr;
    QDoubleSpinBox* m_customAspectWidth = nullptr;
    QDoubleSpinBox* m_customAspectHeight = nullptr;
    QToolButton* m_frameColorButton = nullptr;
    /// 压暗滑条右侧的数值展示（只读，随滑条实时更新）
    QLabel* m_dimValueLabel = nullptr;
    QSlider* m_dimSlider = nullptr;
    QCheckBox* m_maskCheck = nullptr;
    QCheckBox* m_ratioLabelCheck = nullptr;
    QCheckBox* m_thirdsCheck = nullptr;

    /// 右下角一整排扁平五角星（点击 / 悬停预览评级）
    StarRatingWidget* m_starRating = nullptr;
    QPushButton* m_fullScreenButton = nullptr;
    /// 「构图与视图」左侧的「内嵌 JPEG 预览」切换按钮
    QPushButton* m_embeddedJpegButton = nullptr;
    /// 预览下方标记行最左边的「构图与视图」图标按钮（可选中，控制浮层面板）
    QPushButton* m_compositionButton = nullptr;
    /// 浮在预览画面上方的构图与视图调整面板
    QWidget* m_compositionPanel = nullptr;
    /// 左侧栏顶部栏的「设置」图标按钮（可选中，控制设置浮层）
    QPushButton* m_settingsButton = nullptr;
    /// 「设置」右侧的 ICC 模拟器按钮
    QPushButton* m_iccSimButton = nullptr;
    /// 「ICC 模拟器」右侧的帮助按钮
    QPushButton* m_helpButton = nullptr;
    /// 悬浮的 ICC 模拟器窗口（首次点击按钮时创建）
    IccSimulatorWindow* m_iccSimulator = nullptr;
    /// 「帮助」窗口（首次点击按钮时创建，关闭只是隐藏）
    HelpDialog* m_helpDialog = nullptr;
    /// 浮在预览画面上方的设置面板（缩略图管线 / 畸变校正 / 快捷键）
    QWidget* m_settingsPanel = nullptr;
    /// 可在设置里改键的动作 → 当前按键（空序列 = 已清除）
    QHash<QString, QKeySequence> m_shortcuts;
    /// 动作 id → 设置面板里对应的按键按钮（改键 / 重置后刷新文字）
    QHash<QString, ShortcutKeyButton*> m_shortcutButtons;
    /// 「复制并下一张」的窗口级 QShortcut，键位随设置变化
    QShortcut* m_copyShortcut = nullptr;
    /// 标记组两侧的上一张 / 下一张箭头（全屏预览下也显示）
    QPushButton* m_prevImageButton = nullptr;
    QPushButton* m_nextImageButton = nullptr;
    QPushButton* m_copyButton = nullptr;
    QPushButton* m_copyNextButton = nullptr;
    QPushButton* m_copyMarkedButton = nullptr;
};

}  // namespace rawui
