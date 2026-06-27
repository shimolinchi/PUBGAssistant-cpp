#pragma once

#include <QMouseEvent>
#include <QPaintEvent>
#include <QPushButton>

namespace pubg::ui {

// Qt 版圆角按钮，用来还原 Python Tkinter Canvas RoundedButton 的视觉和 toggle 行为。
class RoundedButton : public QPushButton {
    Q_OBJECT
public:
    explicit RoundedButton(const QString& text, QWidget* parent = nullptr);
    static void setThemeColors(const QString& normal, const QString& hover, const QString& pressed,
                               const QString& active, const QString& border, const QString& text);

    // 设置按钮固定尺寸、圆角和字号，便于主窗口按 Python 像素布局复刻。
    void configure(int width, int height, int radius, int pixel_font_size);

    // 设置是否为 toggle 按钮。toggle 按钮点击后保持浅蓝激活态。
    void setToggleMode(bool toggle);

    // 主动设置激活态，不触发 clicked。
    void setActive(bool active);
    void setWarning(bool warning);

    // 返回当前视觉激活态。主窗口用它决定手动助手是否开启。
    [[nodiscard]] bool active() const noexcept { return active_; }

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void refresh();

    bool toggle_mode_ = false;
    bool active_ = false;
    bool hovered_ = false;
    bool pressed_ = false;
    bool warning_ = false;
    int radius_ = 18;
    int pixel_font_size_ = 13;
};

} // namespace pubg::ui
