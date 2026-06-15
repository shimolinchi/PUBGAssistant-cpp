#include "ui/ScaleCalibrationWindow.hpp"

#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QCloseEvent>

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <vector>

#include "ScreenCapture.hpp"
#include "TemplateMatcher.hpp"

namespace pubg::ui {

namespace {

constexpr int kPreviewWidth = 320;
constexpr int kPreviewHeight = 150;
constexpr int kScaleMin = 50;
constexpr int kScaleMax = 220;

QLabel* makePreviewLabel(QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setFixedSize(kPreviewWidth, kPreviewHeight);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(
        "background:#FFFFFF;color:#111827;border:1px solid #CBD5E1;"
        "border-radius:6px;font-size:12px;font-weight:700;"
    );
    return label;
}

void setPreviewPixmap(QLabel* label, const QPixmap& pixmap, const QString& empty_text) {
    if (!label) return;
    if (pixmap.isNull()) {
        label->setPixmap({});
        label->setText(empty_text);
        return;
    }
    label->setText({});
    const QSize target(std::max(1, label->width() - 12), std::max(1, label->height() - 12));
    label->setPixmap(pixmap.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

std::filesystem::path templateRootForKey(const std::filesystem::path& root, const std::string& key) {
    if (key == "weapon_region") return root / "weapons";
    if (key == "weapon1_name_region" || key == "weapon2_name_region") return root / "equipments" / "names";
    if (key == "stance_region") return root / "gestures";
    return root;
}

QString templateDisplayName(const std::filesystem::path& file, const std::filesystem::path& root) {
    if (file.empty()) return QStringLiteral("选择模板");
    std::error_code ec;
    const auto relative = std::filesystem::relative(file, root, ec);
    if (!ec && !relative.empty()) {
        const auto parent = relative.parent_path();
        if (!parent.empty() && parent != ".") {
            return QString::fromStdWString(parent.wstring()) + QStringLiteral(" / ") +
                   QString::fromStdWString(file.stem().wstring());
        }
    }
    return QString::fromStdWString(file.stem().wstring());
}

std::vector<std::filesystem::path> templateFiles(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || ec) return files;
    std::filesystem::recursive_directory_iterator it(root, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
        const auto& entry = *it;
        if (!entry.is_regular_file(ec) || ec) {
            it.increment(ec);
            continue;
        }
        const auto ext = entry.path().extension().string();
        if (ext == ".png" || ext == ".PNG") files.push_back(entry.path());
        it.increment(ec);
    }
    std::sort(files.begin(), files.end());
    return files;
}

cv::Mat toBgr(const cv::Mat& input) {
    if (input.empty()) return {};
    cv::Mat bgr;
    if (input.channels() == 4) {
        cv::cvtColor(input, bgr, cv::COLOR_BGRA2BGR);
    } else if (input.channels() == 1) {
        cv::cvtColor(input, bgr, cv::COLOR_GRAY2BGR);
    } else {
        bgr = input;
    }
    return bgr;
}

cv::Mat outlinePreview(const cv::Mat& input) {
    const cv::Mat bgr = toBgr(input);
    if (bgr.empty()) return {};
    cv::Mat edges = TemplateMatcher::preprocessWeapon(bgr);
    cv::Mat preview(edges.rows, edges.cols, CV_8UC3, cv::Scalar(255, 255, 255));
    preview.setTo(cv::Scalar(17, 24, 39), edges);
    return preview;
}

QPixmap matToPixmap(const cv::Mat& bgr) {
    if (bgr.empty()) return {};
    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    QImage image(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step), QImage::Format_RGB888);
    return QPixmap::fromImage(image.copy());
}

QPixmap outlinePixmapFromFile(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return {};
    const cv::Mat image = cv::imread(path.string(), cv::IMREAD_UNCHANGED);
    if (image.empty()) return {};
    return matToPixmap(outlinePreview(image));
}

int scaledValue(int base, int percent) {
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(base) * percent / 100.0)));
}

struct BoolGuard {
    bool& flag;
    explicit BoolGuard(bool& value) : flag(value) { flag = true; }
    ~BoolGuard() { flag = false; }
};

} // namespace

ScaleCalibrationWindow::ScaleCalibrationWindow(Config& config, RegionManager& regions, QWidget* parent)
    : QWidget(parent), config_(config), regions_(regions) {
    setWindowTitle(QStringLiteral("截图区域缩放比例校准"));
    setObjectName("scaleCalibRoot");
    setStyleSheet(
        "#scaleCalibRoot{background:#FFFFFF;}"
        "QLabel{color:#030712;font-family:'Microsoft YaHei';}"
        "QComboBox,QLineEdit{background:#FFFFFF;color:#030712;border:1px solid #9CA3AF;border-radius:4px;padding:3px 6px;selection-background-color:#DBEAFE;selection-color:#030712;}"
        "QComboBox QAbstractItemView{background:#FFFFFF;color:#030712;border:1px solid #9CA3AF;selection-background-color:#DBEAFE;selection-color:#030712;}"
        "QSlider::groove:horizontal{height:8px;background:#E5E7EB;border-radius:4px;}"
        "QSlider::sub-page:horizontal{background:#93C5FD;border-radius:4px;}"
        "QSlider::handle:horizontal{background:#FFFFFF;border:1px solid #64748B;width:18px;margin:-6px 0;border-radius:9px;}"
        "QPushButton{background:#EEF2F7;color:#030712;border:1px solid #9CA3AF;border-radius:5px;padding:5px 8px;}"
        "QPushButton:hover{background:#E2E8F0;}"
        "QPushButton:pressed{background:#CBD5E1;}"
    );
    setWindowOpacity(0.85);
    resize(710, 390);
    buildUi();
    loadRegion();
    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(220);
    connect(refresh_timer_, &QTimer::timeout, this, &ScaleCalibrationWindow::updatePreview);
    refresh_timer_->start();
}

void ScaleCalibrationWindow::closeEvent(QCloseEvent* event) {
    closing_ = true;
    if (refresh_timer_) {
        refresh_timer_->stop();
        // 彻底断开，避免析构边缘仍有排队的 timeout 触发 updatePreview。
        disconnect(refresh_timer_, nullptr, this, nullptr);
    }
    QWidget::closeEvent(event);
}

void ScaleCalibrationWindow::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(9);

    auto* title = new QLabel(QStringLiteral("截图区域缩放比例校准"), this);
    title->setStyleSheet("font-size:16px;font-weight:700;color:#111827;");
    root->addWidget(title);

    auto* top = new QHBoxLayout();
    top->setSpacing(8);
    region_combo_ = new QComboBox(this);
    region_combo_->addItem(QStringLiteral("武器图标区域"), QStringLiteral("weapon_region"));
    region_combo_->addItem(QStringLiteral("武器1名称区域"), QStringLiteral("weapon1_name_region"));
    region_combo_->addItem(QStringLiteral("武器2名称区域"), QStringLiteral("weapon2_name_region"));
    region_combo_->addItem(QStringLiteral("姿势识别区域"), QStringLiteral("stance_region"));
    template_button_ = new QPushButton(QStringLiteral("选择模板"), this);
    auto* save = new QPushButton(QStringLiteral("保存配置"), this);
    auto* autoBtn = new QPushButton(QStringLiteral("自动校准"), this);
    top->addWidget(region_combo_, 2);
    top->addWidget(template_button_, 3);
    top->addWidget(save);
    top->addWidget(autoBtn);
    root->addLayout(top);

    auto* size_form = new QFormLayout();
    size_form->setHorizontalSpacing(10);
    size_form->setVerticalSpacing(7);
    width_edit_ = new QLineEdit(this);
    height_edit_ = new QLineEdit(this);
    width_edit_->setFixedWidth(72);
    height_edit_->setFixedWidth(72);
    width_slider_ = new QSlider(Qt::Horizontal, this);
    height_slider_ = new QSlider(Qt::Horizontal, this);
    width_slider_->setRange(kScaleMin, kScaleMax);
    height_slider_->setRange(kScaleMin, kScaleMax);
    width_scale_label_ = new QLabel(this);
    height_scale_label_ = new QLabel(this);
    width_scale_label_->setFixedWidth(44);
    height_scale_label_->setFixedWidth(44);

    auto* width_row = new QHBoxLayout();
    width_row->setSpacing(8);
    width_row->addWidget(width_edit_);
    width_row->addWidget(width_slider_, 1);
    width_row->addWidget(width_scale_label_);
    auto* height_row = new QHBoxLayout();
    height_row->setSpacing(8);
    height_row->addWidget(height_edit_);
    height_row->addWidget(height_slider_, 1);
    height_row->addWidget(height_scale_label_);
    size_form->addRow(QStringLiteral("宽度缩放"), width_row);
    size_form->addRow(QStringLiteral("高度缩放"), height_row);
    root->addLayout(size_form);

    auto* preview_row = new QHBoxLayout();
    preview_row->setSpacing(12);
    auto* template_col = new QVBoxLayout();
    template_col->setSpacing(6);
    auto* template_title = new QLabel(QStringLiteral("模板轮廓"), this);
    template_title->setStyleSheet("font-size:13px;font-weight:700;color:#111827;");
    template_preview_ = makePreviewLabel(this);
    template_col->addWidget(template_title);
    template_col->addWidget(template_preview_);

    auto* capture_col = new QVBoxLayout();
    capture_col->setSpacing(6);
    auto* capture_title = new QLabel(QStringLiteral("截图轮廓"), this);
    capture_title->setStyleSheet("font-size:13px;font-weight:700;color:#111827;");
    capture_preview_ = makePreviewLabel(this);
    capture_col->addWidget(capture_title);
    capture_col->addWidget(capture_preview_);
    preview_row->addLayout(template_col);
    preview_row->addLayout(capture_col);
    root->addLayout(preview_row);

    result_label_ = new QLabel(QStringLiteral("截图实时刷新中"), this);
    result_label_->setWordWrap(true);
    result_label_->setStyleSheet("color:#111827;font-size:12px;font-weight:700;");
    root->addWidget(result_label_);

    connect(region_combo_, &QComboBox::currentIndexChanged, this, [this] { loadRegion(); });
    connect(template_button_, &QPushButton::clicked, this, &ScaleCalibrationWindow::chooseTemplate);
    connect(width_slider_, &QSlider::valueChanged, this, &ScaleCalibrationWindow::updateSizeFromSliders);
    connect(height_slider_, &QSlider::valueChanged, this, &ScaleCalibrationWindow::updateSizeFromSliders);
    connect(width_edit_, &QLineEdit::editingFinished, this, [this] {
        const int value = std::max(1, width_edit_->text().toInt());
        const int percent = std::clamp(static_cast<int>(std::lround(value * 100.0 / std::max(1, base_width_))), kScaleMin, kScaleMax);
        QSignalBlocker blocker(width_slider_);
        width_slider_->setValue(percent);
        width_scale_label_->setText(QString::number(percent) + "%");
        updatePreview();
    });
    connect(height_edit_, &QLineEdit::editingFinished, this, [this] {
        const int value = std::max(1, height_edit_->text().toInt());
        const int percent = std::clamp(static_cast<int>(std::lround(value * 100.0 / std::max(1, base_height_))), kScaleMin, kScaleMax);
        QSignalBlocker blocker(height_slider_);
        height_slider_->setValue(percent);
        height_scale_label_->setText(QString::number(percent) + "%");
        updatePreview();
    });
    connect(save, &QPushButton::clicked, this, &ScaleCalibrationWindow::saveConfig);
    connect(autoBtn, &QPushButton::clicked, this, &ScaleCalibrationWindow::runAutoSearch);
}

std::string ScaleCalibrationWindow::currentRegionKey() const {
    return region_combo_->currentData().toString().toStdString();
}

void ScaleCalibrationWindow::loadRegion() {
    const std::string key = currentRegionKey();
    const auto scaling = config_.read([&](const Json& data) {
        return data.value("region_scaling_settings", Json::object()).value(key, Json::object());
    });
    base_width_ = std::max(1, scaling.value("width", 100));
    base_height_ = std::max(1, scaling.value("height", 50));
    {
        QSignalBlocker bw(width_slider_);
        QSignalBlocker bh(height_slider_);
        width_slider_->setValue(100);
        height_slider_->setValue(100);
    }
    width_edit_->setText(QString::number(base_width_));
    height_edit_->setText(QString::number(base_height_));
    width_scale_label_->setText("100%");
    height_scale_label_->setText("100%");
    reloadTemplates();
    updatePreview();
}

void ScaleCalibrationWindow::applySize() {
    const std::string key = currentRegionKey();
    const int width = std::max(1, width_edit_->text().toInt());
    const int height = std::max(1, height_edit_->text().toInt());
    config_.write([&](Json& data) {
        auto& r = data["region_scaling_settings"][key];
        r["width"] = width;
        r["height"] = height;
    });
    base_width_ = width;
    base_height_ = height;
}

void ScaleCalibrationWindow::saveConfig() {
    applySize();
    config_.save();
    {
        QSignalBlocker bw(width_slider_);
        QSignalBlocker bh(height_slider_);
        width_slider_->setValue(100);
        height_slider_->setValue(100);
    }
    width_scale_label_->setText("100%");
    height_scale_label_->setText("100%");
    result_label_->setText(QStringLiteral("配置已保存，截图继续实时刷新。"));
    updatePreview();
}

void ScaleCalibrationWindow::runAutoSearch() {
    result_label_->setText(QStringLiteral("自动校准入口已保留；当前窗口使用实时截图和手动缩放微调。"));
}

void ScaleCalibrationWindow::reloadTemplates() {
    const auto root = templateRootForKey(config_.paths().templatesDir(), currentRegionKey());
    const auto files = templateFiles(root);
    if (files.empty()) {
        current_template_path_.clear();
        template_outline_ = {};
        template_button_->setText(QStringLiteral("未找到模板"));
        return;
    }
    current_template_path_ = files.front();
    template_button_->setText(templateDisplayName(current_template_path_, root));
    loadTemplatePreview();
}

void ScaleCalibrationWindow::chooseTemplate() {
    choosing_template_ = true;
    if (refresh_timer_) refresh_timer_->stop();
    const auto root = templateRootForKey(config_.paths().templatesDir(), currentRegionKey());
    // QFileDialog 会启动嵌套事件循环；用 QPointer 探测对话框期间 this 是否被销毁，
    // 避免对话框返回后访问已析构的窗口成员（Qt6Core use-after-free 崩溃根因）。
    QPointer<ScaleCalibrationWindow> guard(this);
    const QString selected = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择模板"),
        QString::fromStdWString(root.wstring()),
        QStringLiteral("PNG 模板 (*.png)")
    );
    if (!guard || closing_) return;  // 窗口已在对话框期间关闭/销毁。
    choosing_template_ = false;
    if (refresh_timer_) refresh_timer_->start();
    if (selected.isEmpty()) return;
    current_template_path_ = std::filesystem::path(selected.toStdWString());
    template_button_->setText(templateDisplayName(current_template_path_, root));
    loadTemplatePreview();
    updatePreview();
}

void ScaleCalibrationWindow::updateSizeFromSliders() {
    const int width_percent = width_slider_->value();
    const int height_percent = height_slider_->value();
    width_edit_->setText(QString::number(scaledValue(base_width_, width_percent)));
    height_edit_->setText(QString::number(scaledValue(base_height_, height_percent)));
    width_scale_label_->setText(QString::number(width_percent) + "%");
    height_scale_label_->setText(QString::number(height_percent) + "%");
    updatePreview();
}

void ScaleCalibrationWindow::updatePreview() {
    if (closing_ || updating_preview_ || choosing_template_) return;
    BoolGuard guard(updating_preview_);
    const std::string key = currentRegionKey();
    const int width = std::clamp(width_edit_->text().toInt(), 1, 2000);
    const int height = std::clamp(height_edit_->text().toInt(), 1, 2000);
    setPreviewPixmap(template_preview_, template_outline_, QStringLiteral("请选择模板"));
    setPreviewPixmap(capture_preview_, scaledCapturePixmap(key, width, height), QStringLiteral("无法截取当前区域"));
}

void ScaleCalibrationWindow::loadTemplatePreview() {
    template_outline_ = outlinePixmapFromFile(current_template_path_);
}

QPixmap ScaleCalibrationWindow::scaledCapturePixmap(const std::string& key, int width, int height) const {
    try {
        const auto rect = regions_.getRealRegion(key);
        if (!rect || !rect->valid()) return {};
        ScreenCapture capture;
        cv::Mat bgr = capture.grabBgr(*rect);
        if (bgr.empty()) return {};
        cv::Mat resized;
        cv::resize(bgr, resized, cv::Size(width, height), 0.0, 0.0, cv::INTER_AREA);
        return matToPixmap(outlinePreview(resized));
    } catch (const std::exception&) {
        return {};
    } catch (...) {
        return {};
    }
}

} // namespace pubg::ui
