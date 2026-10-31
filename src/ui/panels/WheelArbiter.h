// ============================================================================
//  WheelArbiter.h —— 滚轮调整的激活仲裁
//
//  选图工作台的侧栏里滑块与数值框排列密集，鼠标扫过时滚轮很容易
//  改掉不该改的参数。这里统一规定：左键点中某个滑块 / 数值框后，
//  该控件才响应滚轮；随后点中任何其它位置立即取消激活，
//  同一时刻最多只有一个参数处于激活状态。
// ============================================================================

#pragma once

class QSlider;
class QWheelEvent;
class QWidget;

namespace rawui {

/// 查询某个直接使用的 QSlider 是否已通过点击获得滚轮调整权限。
bool isSliderWheelArmed(const QSlider* slider);

/// 取消某个滑块的滚轮调整权限。双击重置后调用，
/// 避免鼠标仍停在条上时下一次滚动又把刚重置的值改掉。
void disarmSliderWheel(const QSlider* slider);

/// 把未被激活的滚轮事件转交给最近的 QScrollArea 视口。
/// 直接使用 QSlider / QDoubleSpinBox 的面板需要调用它，
/// 否则控件会按原生行为吃掉滚轮并改值。
void forwardWheelToParentScrollArea(QWidget* widget, QWheelEvent* event);

}  // namespace rawui
