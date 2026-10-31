// ============================================================================
//  HelpDialog.h —— 「帮助」窗口：功能介绍、工作流示例、快捷键与关于
//
//  从选图工作台左侧顶部栏（ICC 模拟器右侧）的「帮助」按钮弹出：
//   - 功能一览 + 标准工作流示例：先说明能力，再给出一次完整选片的示例步骤；
//   - 常用快捷键：与内置默认键一致，可在「设置」里改键；
//   - 关于：作者抖音 / 小红书 ID 与邮箱（点击邮箱可直接唤起邮件客户端）。
//
//  非模态窗口，可以一边对照说明一边操作；关掉只是隐藏，下次打开复用。
// ============================================================================

#pragma once

#include <QDialog>

namespace rawui {

class HelpDialog : public QDialog {
    Q_OBJECT

public:
    explicit HelpDialog(QWidget* parent = nullptr);

private:
    void buildUi();
};

}  // namespace rawui
