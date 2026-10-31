// ============================================================================
//  CollapsibleSection.h —— 纵向折叠分组（Lightroom 风格参数面板的基础单元）
//
//  用途：
//   - 参数栏不再用横向页签把所有功能都摊在同一层，而是按功能纵向排列，
//     每组一个可折叠标题条，点标题展开 / 收起；
//   - 同时展开多组，符合"调色时来回对照参数"的习惯；
//   - 可选的设置键让展开状态跨会话保留（与 Lightroom 的面板记忆一致）。
//
//  视觉：
//   - 标题条左侧三角指示状态（▾ 展开 / ▸ 收起），整行可点；
//   - 内容区统一内边距，组与组之间用细分隔线区分，不再画厚重的 GroupBox 边框。
// ============================================================================

#pragma once

#include <QWidget>

class QPushButton;
class QVBoxLayout;

namespace rawui {

class CollapsibleSection : public QWidget {
    Q_OBJECT

public:
    explicit CollapsibleSection(const QString& title, QWidget* parent = nullptr);

    /// 设置内容控件（所有权转移给本分组）
    void setContent(QWidget* content);
    QWidget* content() const { return m_content; }

    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool expanded);

    /// 用 QSettings 记住展开状态。
    /// @param key 形如 "ui/section/style"
    /// @param defaultExpanded 首次使用（还没有存过）时的默认状态
    void setSettingsKey(const QString& key, bool defaultExpanded = true);

    /// 设置分组的一句话说明。
    /// 说明只挂在标题按钮上，不要挂在整个分组控件上：否则鼠标停在
    /// 组内任意子控件（尤其是参数旁的 ⓘ 图标）时，Qt 会把这个父级
    /// tooltip 一并弹出来，和参数自身的说明叠成两层浮窗。
    void setDescription(const QString& description);

    QString title() const { return m_title; }

signals:
    void expandedChanged(bool expanded);

private:
    void refreshHeader();

    QString m_title;
    QString m_description;
    QPushButton* m_header = nullptr;
    QWidget* m_body = nullptr;
    QVBoxLayout* m_bodyLayout = nullptr;
    QWidget* m_content = nullptr;
    QString m_settingsKey;
    bool m_expanded = true;
};

}  // namespace rawui
