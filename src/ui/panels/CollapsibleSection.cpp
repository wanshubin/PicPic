// ============================================================================
//  CollapsibleSection.cpp —— 纵向折叠分组实现
// ============================================================================

#include "CollapsibleSection.h"

#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QSettings>
#include <QSize>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace rawui {

namespace {

/// 绘制展开 / 收起指示用的三角形。
/// 不用 ▾ / ▸ 字符：中文字体（微软雅黑）里没有这些字形，
/// 中文标题会退化成方块，只有纯英文标题能显示成三角。
QPixmap trianglePixmap(bool expanded, const QColor& color)
{
    constexpr int kLogicalSize = 10;
    constexpr qreal kDevicePixelRatio = 2.0;
    QPixmap pixmap(static_cast<int>(kLogicalSize * kDevicePixelRatio),
                   static_cast<int>(kLogicalSize * kDevicePixelRatio));
    pixmap.setDevicePixelRatio(kDevicePixelRatio);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);

    QPolygonF triangle;
    if (expanded) {
        // 向下：顶边水平，底点居中
        triangle << QPointF(2.0, 3.0) << QPointF(8.0, 3.0) << QPointF(5.0, 7.0);
    } else {
        // 向右：左边竖直，右点居中
        triangle << QPointF(3.0, 2.0) << QPointF(7.0, 5.0) << QPointF(3.0, 8.0);
    }
    painter.drawPolygon(triangle);
    return pixmap;
}

QIcon triangleIcon(bool expanded)
{
    QIcon icon;
    // 标题按钮是 checkable 的，按下 / 松开两种状态都提供同一套图形，
    // 避免 Qt 因状态不匹配而回退成一张模糊的缩放图。
    for (const QIcon::State state : {QIcon::Off, QIcon::On}) {
        icon.addPixmap(trianglePixmap(expanded, QColor(0x9a, 0xa0, 0xa6)),
                       QIcon::Normal, state);
        icon.addPixmap(trianglePixmap(expanded, QColor(0xe6, 0xe6, 0xe6)),
                       QIcon::Active, state);
        icon.addPixmap(trianglePixmap(expanded, QColor(0x6a, 0x6a, 0x6a)),
                       QIcon::Disabled, state);
    }
    return icon;
}

}  // namespace

CollapsibleSection::CollapsibleSection(const QString& title, QWidget* parent)
    : QWidget(parent)
    , m_title(title)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---- 标题条：整行可点，左侧三角表示展开状态 ----
    // 比内容区亮一档的窄条，像 Lightroom 的分组标题：
    // 高度压到约 26px，只靠字重和亮度区分层级，不加多余装饰。
    m_header = new QPushButton(this);
    m_header->setCheckable(true);
    m_header->setChecked(true);
    m_header->setFlat(true);
    m_header->setCursor(Qt::PointingHandCursor);
    // 不抢焦点：键盘操作始终留在画布 / 滑块上
    m_header->setFocusPolicy(Qt::NoFocus);
    m_header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_header->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  text-align: left;"
        "  padding: 5px 10px;"
        "  border: none;"
        "  border-top: 1px solid #1f1f1f;"
        "  border-bottom: 1px solid #2e2e2e;"
        "  background: #343434;"
        "  color: #d6d6d6;"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background: #3d3d3d; color: #f0f0f0; }"
        "QPushButton:pressed { background: #303030; }"));
    layout->addWidget(m_header);

    // ---- 内容区 ----
    m_body = new QWidget(this);
    m_body->setObjectName(QStringLiteral("CollapsibleSectionBody"));
    m_body->setStyleSheet(QStringLiteral(
        "#CollapsibleSectionBody { background: #262626; }"));
    m_bodyLayout = new QVBoxLayout(m_body);
    // 左右留出完整的呼吸空间，上下收紧：单行滑块本身已占 22px，
    // 行间再留 6px 既能分组，又不会把参数拉得过于稀疏。
    m_bodyLayout->setContentsMargins(12, 6, 12, 10);
    m_bodyLayout->setSpacing(6);
    layout->addWidget(m_body);

    refreshHeader();

    connect(m_header, &QPushButton::clicked, this, [this]() {
        // 可勾选按钮点击时已自动翻转勾选状态，这里只负责应用
        setExpanded(m_header->isChecked());
    });
}

void CollapsibleSection::setContent(QWidget* content)
{
    if (content == nullptr) {
        return;
    }
    if (m_content != nullptr) {
        m_bodyLayout->removeWidget(m_content);
        m_content->deleteLater();
    }
    m_content = content;
    m_bodyLayout->addWidget(content);
}

void CollapsibleSection::setExpanded(bool expanded)
{
    m_expanded = expanded;
    m_body->setVisible(expanded);
    refreshHeader();

    if (!m_settingsKey.isEmpty()) {
        QSettings().setValue(m_settingsKey, expanded);
    }
    emit expandedChanged(expanded);
}

void CollapsibleSection::setSettingsKey(const QString& key, bool defaultExpanded)
{
    m_settingsKey = key;
    // 首次使用还没有存过状态时，采用调用方给的默认值
    const bool expanded = QSettings().value(key, defaultExpanded).toBool();
    setExpanded(expanded);
}

void CollapsibleSection::setDescription(const QString& description)
{
    m_description = description;
    refreshHeader();
}

void CollapsibleSection::refreshHeader()
{
    // 勾选状态与展开状态保持一致，但不要再次触发 clicked
    if (m_header->isChecked() != m_expanded) {
        const QSignalBlocker blocker(m_header);
        m_header->setChecked(m_expanded);
    }
    // 三角用绘制图标而不是 ▾ / ▸ 字符：中文标题会因字体缺字显示成方块
    m_header->setIcon(triangleIcon(m_expanded));
    m_header->setIconSize(QSize(10, 10));
    m_header->setText(m_title);

    // 分组说明只挂在标题按钮上，便于悬停标题时了解这一组管什么。
    // 不要用 setToolTip() 挂到整个分组：组内的 ⓘ 图标有自己的说明弹窗，
    // 父级 tooltip 会在悬停图标时被 Qt 一并弹出，形成"详细说明 + 一句话"
    // 两层浮窗。
    QString tooltip = m_expanded ? QStringLiteral("收起「%1」").arg(m_title)
                                 : QStringLiteral("展开「%1」").arg(m_title);
    if (!m_description.isEmpty()) {
        tooltip = m_description + QLatin1Char('\n') + tooltip;
    }
    m_header->setToolTip(tooltip);
}

}  // namespace rawui
