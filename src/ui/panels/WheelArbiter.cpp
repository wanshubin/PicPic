// ============================================================================
//  WheelArbiter.cpp —— 滚轮调整的激活仲裁实现
// ============================================================================

#include "WheelArbiter.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QScrollArea>
#include <QSlider>
#include <QWheelEvent>

namespace rawui {

/// 把滚轮事件转交给最近的滚动区域视口。
/// 控件未被激活时，滚轮应当继续滚动侧栏；
/// 不能直接放行给控件自身，否则滑块 / 输入框会按原生行为改值。
void forwardWheelToParentScrollArea(QWidget* widget, QWheelEvent* event)
{
    for (QWidget* parent = widget->parentWidget(); parent != nullptr;
         parent = parent->parentWidget()) {
        if (auto* area = qobject_cast<QScrollArea*>(parent)) {
            QCoreApplication::sendEvent(area->viewport(), event);
            return;
        }
    }
}

namespace {

// ===========================================================================
//  SliderWheelArbiter —— 滚轮调整的激活仲裁
//
//  必须放在应用级事件过滤器里：点击发生在目标控件之外时，
//  目标控件自身收不到事件，无从得知"用户已经去看别处了"。
// ===========================================================================

class SliderWheelArbiter : public QObject
{
public:
    explicit SliderWheelArbiter(QObject* parent) : QObject(parent) {}

    static SliderWheelArbiter* instance()
    {
        static SliderWheelArbiter* arbiter = nullptr;
        if (arbiter == nullptr && qApp != nullptr) {
            arbiter = new SliderWheelArbiter(qApp);
            qApp->installEventFilter(arbiter);
        }
        return arbiter;
    }

    /// 该滑块当前是否拥有滚轮调整权限
    bool isArmed(const QSlider* slider) const
    {
        return slider != nullptr && m_armedSlider == slider;
    }

    /// 主动取消某个滑块的滚轮权限（双击重置后调用）
    void disarm(const QSlider* slider)
    {
        if (slider != nullptr && m_armedSlider == slider) {
            setArmedSlider(nullptr);
        }
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Wheel) {
            return filterDirectInputWheel(watched, event);
        }
        if (event->type() != QEvent::MouseButtonPress) {
            return false;
        }

        // 只处理发到控件上的事件：平台层的鼠标事件会先发给 QWindow，
        // 随后还会以 QWidget 事件再发一次，看前者只会白白抖动一次状态
        auto* pressed = qobject_cast<QWidget*>(watched);
        if (pressed == nullptr) {
            return false;
        }

        // 左键点中任意滑块后，该滑块获得滚轮权限；
        // 下拉框 / 数值框也走同一套规则；点击其它位置立即取消。
        auto* mouse = static_cast<QMouseEvent*>(event);
        const bool leftButton = mouse->button() == Qt::LeftButton;
        if (QSlider* pressedSlider = sliderForPress(pressed)) {
            if (leftButton) {
                setArmedSlider(pressedSlider);
            }
        } else {
            setArmedInput(leftButton ? directInputForPress(pressed) : nullptr);
        }
        return false;
    }

private:
    /// 下拉框 / 数值框的滚轮拦截。
    /// 未点中时把滚轮转交给侧栏，避免鼠标扫过选项时误改；
    /// 已点中则放行给控件自身的原生行为。
    bool filterDirectInputWheel(QObject* watched, QEvent* event)
    {
        auto* target = qobject_cast<QWidget*>(watched);
        if (target == nullptr) {
            return false;
        }
        QWidget* input = directInputForWheel(target);
        if (input == nullptr) {
            return false;
        }
        // 下拉列表 / 菜单已经弹出时，滚轮属于弹窗本身（滚动列表），不拦截
        if (QApplication::activePopupWidget() != nullptr) {
            return false;
        }
        if (m_armedInput == input) {
            return false;
        }
        forwardWheelToParentScrollArea(input, static_cast<QWheelEvent*>(event));
        return true;
    }

    /// 从被点击的控件向上找直接使用的下拉框 / 数值框
    static QWidget* directInputForPress(QWidget* pressed)
    {
        QWidget* input = nullptr;
        for (QWidget* widget = pressed; widget != nullptr;
             widget = widget->parentWidget()) {
            if (qobject_cast<QSlider*>(widget) != nullptr) {
                return nullptr;
            }
            if (input == nullptr
                && (qobject_cast<QComboBox*>(widget) != nullptr
                    || qobject_cast<QAbstractSpinBox*>(widget) != nullptr)) {
                input = widget;
            }
        }
        return input;
    }

    /// 滚轮目标是否属于侧栏里的直接下拉框 / 数值框。
    /// 只接管 QScrollArea 内的控件：文件对话框等区域保持系统默认行为。
    static QWidget* directInputForWheel(QWidget* target)
    {
        QWidget* input = nullptr;
        for (QWidget* widget = target; widget != nullptr;
             widget = widget->parentWidget()) {
            if (qobject_cast<QSlider*>(widget) != nullptr) {
                return nullptr;
            }
            if (input == nullptr
                && (qobject_cast<QComboBox*>(widget) != nullptr
                    || qobject_cast<QAbstractSpinBox*>(widget) != nullptr)) {
                input = widget;
            }
            if (qobject_cast<QScrollArea*>(widget) != nullptr) {
                return input;
            }
        }
        return nullptr;
    }

    /// 从被点击的控件向上找滑块
    static QSlider* sliderForPress(QWidget* pressed)
    {
        for (QWidget* widget = pressed; widget != nullptr;
             widget = widget->parentWidget()) {
            if (auto* slider = qobject_cast<QSlider*>(widget)) {
                return slider;
            }
        }
        return nullptr;
    }

    void setArmedSlider(QSlider* slider)
    {
        if (m_armedSlider == slider && m_armedInput == nullptr) {
            return;
        }
        m_armedInput = nullptr;
        m_armedSlider = slider;
    }

    /// 激活 / 取消一个直接使用的下拉框或数值框
    void setArmedInput(QWidget* input)
    {
        if (m_armedInput == input && m_armedSlider == nullptr) {
            return;
        }
        m_armedSlider = nullptr;
        m_armedInput = input;
    }

    // 用 QPointer：侧栏重建时滑块可能先于本对象销毁
    QPointer<QSlider> m_armedSlider;
    QPointer<QWidget> m_armedInput;
};

}  // namespace

/// 查询某个直接使用的 QSlider 是否已通过点击获得滚轮调整权限
bool isSliderWheelArmed(const QSlider* slider)
{
    SliderWheelArbiter* arbiter = SliderWheelArbiter::instance();
    return arbiter != nullptr && arbiter->isArmed(slider);
}

/// 取消某个滑块的滚轮调整权限
void disarmSliderWheel(const QSlider* slider)
{
    if (SliderWheelArbiter* arbiter = SliderWheelArbiter::instance()) {
        arbiter->disarm(slider);
    }
}

}  // namespace rawui
