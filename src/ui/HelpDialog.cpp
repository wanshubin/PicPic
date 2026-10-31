// ============================================================================
//  HelpDialog.cpp —— 「帮助」窗口实现
//
//  排版沿用主界面的深色中性配色：
//   - 标题 #ececec，分组标题 #8f8f8f，正文 #c0c0c0，次要说明 #7a7a7a；
//   - 正文放进滚动区，窗口压矮时也能看全，不会被下沿裁掉；
//   - 「关于」的社媒 ID 旁放一张圆形头像，邮箱只显示文本、不做成链接。
// ============================================================================

#include "HelpDialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>

namespace rawui {

namespace {

/// 分组标题（「功能一览」「标准工作流示例」「常用快捷键」「关于」）
QLabel* makeSectionTitle(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
    label->setStyleSheet(
        QStringLiteral("color: #8f8f8f; background: transparent;"));
    return label;
}

/// 步骤的标题行，例如「① 选择源目录」
QLabel* makeStepTitle(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(
        QStringLiteral("color: #e3e3e3; font-weight: 600; background: transparent;"));
    return label;
}

/// 正文段落。宽度跟随滚动区，自动换行。
QLabel* makeBodyText(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setStyleSheet(
        QStringLiteral("color: #b8b8b8; background: transparent;"));
    return label;
}

/// 分组之间的细分隔线，与设置面板使用同一分隔语言
QWidget* makeDivider(QWidget* parent)
{
    auto* divider = new QWidget(parent);
    divider->setFixedHeight(1);
    divider->setStyleSheet(QStringLiteral("background: #3a3a3a;"));
    return divider;
}

}  // namespace

HelpDialog::HelpDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("帮助"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
    setModal(false);
    setMinimumSize(460, 420);

    // 默认尺寸控制在屏幕可用区域内，小屏幕上也放得下
    int targetWidth = 580;
    int targetHeight = 660;
    if (QScreen* screen = this->screen()) {
        const QRect available = screen->availableGeometry();
        targetWidth = qMin(targetWidth, available.width() - 100);
        targetHeight = qMin(targetHeight, available.height() - 100);
    }
    resize(targetWidth, targetHeight);

    buildUi();
}

void HelpDialog::buildUi()
{
    setStyleSheet(QStringLiteral("QDialog { background: #232323; }"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(22, 18, 22, 16);
    outer->setSpacing(6);

    // ---- 标题 ----
    auto* title = new QLabel(QStringLiteral("PicPic 使用帮助"), this);
    QFont titleFont = title->font();
    titleFont.setPixelSize(19);
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    title->setStyleSheet(QStringLiteral("color: #ececec; background: transparent;"));
    outer->addWidget(title);

    auto* subtitle = new QLabel(
        QStringLiteral("快速选图工作台　·　版本 %1")
            .arg(QCoreApplication::applicationVersion()),
        this);
    subtitle->setStyleSheet(QStringLiteral("color: #7a7a7a; background: transparent;"));
    outer->addWidget(subtitle);
    outer->addSpacing(4);

    // ---- 滚动内容 ----
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollArea > QWidget > QWidget { background: transparent; }"));
    scroll->viewport()->setAutoFillBackground(false);

    auto* body = new QWidget(scroll);
    body->setObjectName(QStringLiteral("HelpBody"));
    body->setStyleSheet(
        QStringLiteral("QWidget#HelpBody { background: transparent; }"));
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 12, 8);
    layout->setSpacing(8);

    // ===== 功能一览 =====
    layout->addWidget(makeSectionTitle(QStringLiteral("功能一览"), body));

    struct Feature {
        const char* title;
        const char* detail;
    };
    const Feature features[] = {
        {"浏览与选片",
         "缩略图网格配大图预览，F 键全屏；缩略图默认读取相机内嵌 JPEG，"
         "主预览可在相机内嵌 JPEG 与软件渲染管线之间切换。"},
        {"筛选与排序",
         "按格式（全部 / RAW / 普通图片）、星级、颜色标记筛选；按文件名 / "
         "拍摄时间 / 文件大小排序并可降序；「时间分组」按相邻拍摄间隔自动分栏"
         "（1 分钟 ~ 1 天），时间相近的照片归到同一组。"},
        {"评级与标记",
         "0~5 星评级、颜色标记（X 清除）；最多 10 个目标目录，每个目录对应一种颜色，"
         "预览下方的颜色圆点与目录一一对应。"},
        {"分拣复制",
         "逐张复制、复制并下一张、批量复制所有带标记的图片；目标目录已有内容相同的"
         "副本时跳过，不会覆盖；每次动作写入「分拣日志」。"},
        {"二次构图",
         "固定视野取景框，预设 / 自定义比例、框线颜色、框外压暗、三分构图线；"
         "支持仅预览旋转；「截取构图视野」按取景框保存 JPG，不改原片。"},
        {"批量工具",
         "导出选片 CSV（星级、标记、目标目录、完整 EXIF 与 GPS）、按前缀批量重命名、"
         "为选中的图片创建副本。"},
        {"设置与显示",
         "「设置」里可切换缩略图展示为RAW、开关镜头畸变校正、自定义快捷键；"
         "ICC 模拟器悬浮窗可模拟亮度、对比度、饱和度、色温与伽马。"},
        {"数据与安全",
         "星级与颜色标记只写入源目录旁的 .rawedit.pick.json 侧车文件；"
         "不修改 RAW / EXIF，复制不覆盖目标目录里的已有文件。"},
    };
    for (const Feature& feature : features) {
        layout->addSpacing(2);
        layout->addWidget(makeStepTitle(QString::fromUtf8(feature.title), body));
        layout->addWidget(makeBodyText(QString::fromUtf8(feature.detail), body));
    }

    layout->addSpacing(6);
    layout->addWidget(makeDivider(body));
    layout->addSpacing(6);

    // ===== 标准工作流示例 =====
    layout->addWidget(makeSectionTitle(QStringLiteral("标准工作流示例"), body));
    layout->addWidget(makeBodyText(
        QStringLiteral("以一次旅行拍摄后的选片为例："), body));

    struct Step {
        const char* title;
        const char* detail;
    };
    const Step steps[] = {
        {"① 打开这次拍摄的文件夹",
         "选择相机 / 手机导出的照片目录；照片分散在子目录时勾选「包含子文件夹」。"},
        {"② 先快速过一遍",
         "用 ← → 逐张看，F 全屏，按 0~5 打星；同一场景可以打开「时间分组」按组比较。"},
        {"③ 标记要留下的照片",
         "给「精选」「待修」等目标目录，看到要留的图点颜色圆点标记，"
         "按空格复制并跳到下一张。"},
        {"④ 批量交付",
         "全部过完后点「批量复制标记图片到对应目录」，一次复制到各自目录。"},
        {"⑤ 复盘归档",
         "在「分拣日志」核对复制记录，导出 CSV 交给修图流程；"
         "需要留档时截取构图视野。"},
    };
    for (const Step& step : steps) {
        layout->addSpacing(2);
        layout->addWidget(makeStepTitle(QString::fromUtf8(step.title), body));
        layout->addWidget(makeBodyText(QString::fromUtf8(step.detail), body));
    }

    layout->addSpacing(4);
    layout->addWidget(makeBodyText(
        QStringLiteral("星级与颜色标记只写入源目录旁的 .rawedit.pick.json，"
                       "原片不会被修改；构图框与旋转只影响预览，不会生成裁切文件。"),
        body));

    layout->addSpacing(6);
    layout->addWidget(makeDivider(body));
    layout->addSpacing(6);

    // ===== 常用快捷键 =====
    layout->addWidget(makeSectionTitle(QStringLiteral("常用快捷键"), body));
    layout->addWidget(makeBodyText(
        QStringLiteral("与内置默认键一致，可在「设置」面板里修改或清除。"), body));

    auto* keyGrid = new QGridLayout;
    keyGrid->setContentsMargins(0, 2, 0, 0);
    keyGrid->setHorizontalSpacing(10);
    keyGrid->setVerticalSpacing(4);

    struct KeyHint {
        const char* keys;
        const char* action;
    };
    const KeyHint keyHints[] = {
        {"← / →", "上一张 / 下一张"},
        {"↑ / ↓", "上一行 / 下一行"},
        {"空格", "复制并下一张（长按 1 秒弹出色板选择标记颜色）"},
        {"0 ~ 5", "设置星级，0 清除"},
        {"X", "清除颜色标记"},
        {"R", "开关固定视野"},
        {"M", "开关框外压暗"},
        {"F", "全屏 / 退出全屏"},
    };
    int row = 0;
    for (const KeyHint& hint : keyHints) {
        auto* keyLabel = new QLabel(QString::fromUtf8(hint.keys), body);
        keyLabel->setStyleSheet(
            QStringLiteral("color: #d8d8d8; font-weight: 600; background: transparent;"));
        auto* actionLabel = new QLabel(QString::fromUtf8(hint.action), body);
        actionLabel->setWordWrap(true);
        actionLabel->setStyleSheet(
            QStringLiteral("color: #b0b0b0; background: transparent;"));
        keyGrid->addWidget(keyLabel, row, 0, Qt::AlignTop);
        keyGrid->addWidget(actionLabel, row, 1);
        ++row;
    }
    keyGrid->setColumnStretch(1, 1);
    layout->addLayout(keyGrid);

    layout->addSpacing(6);
    layout->addWidget(makeDivider(body));
    layout->addSpacing(6);

    // ===== 关于 =====
    layout->addWidget(makeSectionTitle(QStringLiteral("关于"), body));

    // 社媒 ID 右侧放作者头像：略大于一行文字，且整行不换行
    auto* aboutRow = new QHBoxLayout;
    aboutRow->setContentsMargins(0, 0, 0, 0);
    aboutRow->setSpacing(8);
    auto* socialLabel =
        new QLabel(QStringLiteral("抖音 / 小红书 ID：二仙桥热心市民"), body);
    // 不自动换行：避免和头像抢宽度时被截成两行
    socialLabel->setWordWrap(false);
    socialLabel->setStyleSheet(
        QStringLiteral("color: #b8b8b8; background: transparent;"));
    aboutRow->addWidget(socialLabel, 0, Qt::AlignVCenter);

    auto* avatar = new QLabel(body);
    // 用 QIcon::pixmap 按屏幕缩放比例取图，高 DPI 下也是清晰的圆形
    avatar->setPixmap(QIcon(QStringLiteral(":/icons/avatar.png"))
                          .pixmap(QSize(20, 20), avatar->devicePixelRatioF()));
    avatar->setFixedSize(20, 20);
    avatar->setStyleSheet(QStringLiteral("background: transparent;"));
    aboutRow->addWidget(avatar, 0, Qt::AlignVCenter);
    aboutRow->addStretch(1);
    layout->addLayout(aboutRow);

    // 邮箱只作为文本展示，不提供点击发送
    layout->addWidget(makeBodyText(
        QStringLiteral("邮箱：1563535490@qq.com"), body));

    layout->addWidget(makeBodyText(
        QStringLiteral("欢迎提供支持和建议，你的反馈会让它更好用。"), body));
    layout->addStretch(1);

    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    // ---- 底部：关闭 ----
    auto* buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    auto* closeButton = new QPushButton(QStringLiteral("关闭"), this);
    closeButton->setFocusPolicy(Qt::NoFocus);
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setMinimumWidth(76);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    buttonRow->addWidget(closeButton);
    outer->addLayout(buttonRow);
}

}  // namespace rawui
