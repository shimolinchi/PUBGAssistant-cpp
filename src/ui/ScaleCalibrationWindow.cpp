#include "ui/ScaleCalibrationWindow.hpp"

#include <QCloseEvent>
#include <QFormLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QImage>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <vector>

#include "ScreenCapture.hpp"
#include "TemplateMatcher.hpp"
#include "ui/Theme.hpp"

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
            if (file.stem() == "0") return QString::fromStdWString(parent.wstring());
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

cv::Mat toGray(const cv::Mat& input) {
    if (input.empty()) return {};
    cv::Mat gray;
    if (input.channels() == 4) {
        cv::cvtColor(input, gray, cv::COLOR_BGRA2GRAY);
    } else if (input.channels() == 3) {
        cv::cvtColor(input, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = input;
    }
    return gray;
}

cv::Mat binaryPreview(const cv::Mat& binary) {
    if (binary.empty()) return {};
    cv::Mat mask;
    if (binary.type() == CV_8U) {
        mask = binary;
    } else {
        binary.convertTo(mask, CV_8U);
    }
    cv::Mat preview(mask.rows, mask.cols, CV_8UC3, cv::Scalar(255, 255, 255));
    preview.setTo(cv::Scalar(17, 24, 39), mask);
    return preview;
}

cv::Mat grayPreview(const cv::Mat& input) {
    const cv::Mat gray = toGray(input);
    if (gray.empty()) return {};
    cv::Mat bgr;
    cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);
    return bgr;
}

cv::Mat weaponOutlinePreview(const cv::Mat& input) {
    const cv::Mat bgr = toBgr(input);
    if (bgr.empty()) return {};
    const cv::Mat edges = TemplateMatcher::preprocessWeapon(bgr);
    return binaryPreview(edges);
}

cv::Mat whiteTextPreview(const cv::Mat& input) {
    const cv::Mat bgr = toBgr(input);
    if (bgr.empty()) return {};
    cv::Mat binary;
    cv::inRange(bgr, cv::Scalar(220, 220, 220), cv::Scalar(255, 255, 255), binary);
    return binaryPreview(binary);
}

cv::Mat matchMatForRegion(const cv::Mat& input, const std::string& key) {
    if (key == "weapon_region") return TemplateMatcher::preprocessWeapon(toBgr(input));
    if (key == "weapon1_name_region" || key == "weapon2_name_region") return toGray(input);
    if (key == "stance_region") return toGray(input);
    return toGray(input);
}

cv::Mat previewForRegion(const cv::Mat& input, const std::string& key) {
    if (key == "weapon_region") return weaponOutlinePreview(input);
    if (key == "weapon1_name_region" || key == "weapon2_name_region") return whiteTextPreview(input);
    if (key == "stance_region") return weaponOutlinePreview(input);
    return grayPreview(input);
}

std::optional<double> matchScoreForRegion(const cv::Mat& roi, const cv::Mat& tpl, const std::string& key) {
    if (roi.empty() || tpl.empty() || tpl.rows > roi.rows || tpl.cols > roi.cols) return std::nullopt;
    cv::Mat roi_match = matchMatForRegion(roi, key);
    cv::Mat tpl_match = matchMatForRegion(tpl, key);
    if (roi_match.empty() || tpl_match.empty() || tpl_match.rows > roi_match.rows || tpl_match.cols > roi_match.cols) {
        return std::nullopt;
    }

    cv::Mat result;
    if (key == "weapon_region") {
        cv::Mat mask;
        cv::dilate(tpl_match, mask, cv::Mat::ones(5, 5, CV_8U), {-1, -1}, 1);
        if (cv::countNonZero(mask) == 0 || cv::countNonZero(roi_match) < 5) return std::nullopt;
        cv::matchTemplate(roi_match, tpl_match, result, cv::TM_CCORR_NORMED, mask);
    } else {
        cv::matchTemplate(roi_match, tpl_match, result, cv::TM_CCOEFF_NORMED);
    }

    double max_val = 0.0;
    cv::minMaxLoc(result, nullptr, &max_val);
    if (!std::isfinite(max_val)) return std::nullopt;
    return max_val;
}

QPixmap matToPixmap(const cv::Mat& bgr) {
    if (bgr.empty()) return {};
    cv::Mat contiguous = bgr.isContinuous() ? bgr : bgr.clone();
    cv::Mat rgb;
    cv::cvtColor(contiguous, rgb, cv::COLOR_BGR2RGB);
    rgb = rgb.clone();
    QImage image(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step), QImage::Format_RGB888);
    return QPixmap::fromImage(image.copy());
}

QPixmap previewPixmapFromFile(const std::filesystem::path& path, const std::string& key) {
    if (path.empty()) return {};
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return {};
    const cv::Mat image = cv::imread(path.string(), cv::IMREAD_UNCHANGED);
    if (image.empty()) return {};
    return matToPixmap(previewForRegion(image, key));
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
    resize(710, 390);
    applyThemedPopupWindow(this, config_);
    setStyleSheet(styleSheet() + themedSliderStyle(currentUiTheme(config_)));
    buildUi();
    loadRegion();
    refresh_timer_ = new QTimer(this);
    refresh_timer_->setInterval(220);
    connect(refresh_timer_, &QTimer::timeout, this, &ScaleCalibrationWindow::updatePreview);
    startRefresh();
}

void ScaleCalibrationWindow::closeEvent(QCloseEvent* event) {
    closing_ = true;
    stopRefresh();
    if (template_preview_) template_preview_->clear();
    if (capture_preview_) capture_preview_->clear();
    template_outline_ = {};
    event->ignore();
    hide();
}

void ScaleCalibrationWindow::hideEvent(QHideEvent* event) {
    closing_ = true;
    stopRefresh();
    QWidget::hideEvent(event);
}

void ScaleCalibrationWindow::showEvent(QShowEvent* event) {
    closing_ = false;
    QWidget::showEvent(event);
    startRefresh();
    updatePreview();
}

void ScaleCalibrationWindow::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(9);

    auto* title = new QLabel(QStringLiteral("截图区域缩放比例校准"), this);
    const auto theme = currentUiTheme(config_);
    title->setStyleSheet(QStringLiteral("font-size:16px;font-weight:700;color:%1;").arg(theme.button_text));
    root->addWidget(title);

    auto* top = new QHBoxLayout();
    top->setSpacing(8);
    region_combo_ = new QComboBox(this);
    region_combo_->addItem(QStringLiteral("武器图标区域"), QStringLiteral("weapon_region"));
    region_combo_->addItem(QStringLiteral("武器1名称区域"), QStringLiteral("weapon1_name_region"));
    region_combo_->addItem(QStringLiteral("武器2名称区域"), QStringLiteral("weapon2_name_region"));
    region_combo_->addItem(QStringLiteral("姿势识别区域"), QStringLiteral("stance_region"));
    template_combo_ = new QComboBox(this);
    auto* save = new QPushButton(QStringLiteral("保存配置"), this);
    auto* autoBtn = new QPushButton(QStringLiteral("自动校准"), this);
    top->addWidget(region_combo_, 2);
    top->addWidget(template_combo_, 3);
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
    auto* template_title = new QLabel(QStringLiteral("模板处理图"), this);
    template_title->setStyleSheet(QStringLiteral("font-size:13px;font-weight:700;color:%1;").arg(theme.button_text));
    template_preview_ = makePreviewLabel(this);
    template_preview_->setStyleSheet(QStringLiteral("%1font-size:12px;font-weight:700;").arg(themedPanelStyle(theme)));
    template_col->addWidget(template_title);
    template_col->addWidget(template_preview_);

    auto* capture_col = new QVBoxLayout();
    capture_col->setSpacing(6);
    auto* capture_title = new QLabel(QStringLiteral("截图处理图"), this);
    capture_title->setStyleSheet(QStringLiteral("font-size:13px;font-weight:700;color:%1;").arg(theme.button_text));
    capture_preview_ = makePreviewLabel(this);
    capture_preview_->setStyleSheet(QStringLiteral("%1font-size:12px;font-weight:700;").arg(themedPanelStyle(theme)));
    capture_col->addWidget(capture_title);
    capture_col->addWidget(capture_preview_);
    preview_row->addLayout(template_col);
    preview_row->addLayout(capture_col);
    root->addLayout(preview_row);

    result_label_ = new QLabel(QStringLiteral("截图实时刷新中"), this);
    result_label_->setWordWrap(true);
    result_label_->setStyleSheet(QStringLiteral("color:%1;font-size:12px;font-weight:700;").arg(theme.button_text));
    root->addWidget(result_label_);

    connect(region_combo_, &QComboBox::currentIndexChanged, this, [this] { loadRegion(); });
    connect(template_combo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || index >= static_cast<int>(template_paths_.size())) {
            current_template_path_.clear();
            template_outline_ = {};
        } else {
            current_template_path_ = template_paths_[static_cast<size_t>(index)];
            loadTemplatePreview();
        }
        updatePreview();
    });
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
    refreshRegionComboWarnings();
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
    refreshRegionComboWarnings();
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

void ScaleCalibrationWindow::refreshRegionComboWarnings() {
    if (!region_combo_) return;
    const int current = region_combo_->currentIndex();
    const auto settings = config_.read([](const Json& data) {
        return data.value("region_scaling_settings", Json::object());
    });
    for (int i = 0; i < region_combo_->count(); ++i) {
        const QString key = region_combo_->itemData(i).toString();
        QString text = region_combo_->itemText(i);
        text.remove(QStringLiteral("! "));
        const auto k = key.toStdString();
        const bool missing = !settings.contains(k) || !settings[k].is_object() ||
            settings[k].value("width", 0) <= 0 || settings[k].value("height", 0) <= 0;
        region_combo_->setItemText(i, missing ? QStringLiteral("! ") + text : text);
    }
    region_combo_->setCurrentIndex(current);
}

void ScaleCalibrationWindow::runAutoSearch() {
    if (current_template_path_.empty()) {
        result_label_->setText(QStringLiteral("没有可用于自动校准的模板。"));
        return;
    }

    const std::string key = currentRegionKey();
    const auto rect = regions_.getRealRegion(key);
    if (!rect || !rect->valid()) {
        result_label_->setText(QStringLiteral("无法读取当前区域，请先校准截图区域。"));
        return;
    }

    cv::Mat source;
    cv::Mat tpl;
    try {
        ScreenCapture capture;
        source = capture.grabBgr(*rect);
        tpl = cv::imread(current_template_path_.string(), cv::IMREAD_UNCHANGED);
    } catch (...) {
        result_label_->setText(QStringLiteral("截图失败，无法自动校准。"));
        return;
    }
    if (source.empty() || tpl.empty()) {
        result_label_->setText(QStringLiteral("截图或模板为空，无法自动校准。"));
        return;
    }

    auto scoreAt = [&](int w, int h) -> std::optional<double> {
        cv::Mat resized;
        cv::resize(source, resized, cv::Size(w, h), 0.0, 0.0, cv::INTER_AREA);
        return matchScoreForRegion(resized, tpl, key);
    };

    int best_w = std::clamp(width_edit_->text().toInt(), 1, 2000);
    int best_h = std::clamp(height_edit_->text().toInt(), 1, 2000);
    double best_score = -std::numeric_limits<double>::infinity();

    auto consider = [&](int w, int h) {
        if (w <= 0 || h <= 0 || w > 2000 || h > 2000) return;
        const auto score = scoreAt(w, h);
        if (score && *score > best_score) {
            best_score = *score;
            best_w = w;
            best_h = h;
        }
    };

    const int start_w = std::max(1, width_edit_->text().toInt());
    const int start_h = std::max(1, height_edit_->text().toInt());
    const int min_w = std::max(1, scaledValue(base_width_, kScaleMin));
    const int max_w = std::min(2000, scaledValue(base_width_, kScaleMax));
    const int min_h = std::max(1, scaledValue(base_height_, kScaleMin));
    const int max_h = std::min(2000, scaledValue(base_height_, kScaleMax));

    const int coarse_w_step = std::max(1, base_width_ / 20);
    const int coarse_h_step = std::max(1, base_height_ / 20);
    for (int w = min_w; w <= max_w; w += coarse_w_step) {
        for (int h = min_h; h <= max_h; h += coarse_h_step) {
            consider(w, h);
        }
    }
    consider(start_w, start_h);
    consider(max_w, max_h);

    const int fine_w_min = std::max(min_w, best_w - coarse_w_step * 2);
    const int fine_w_max = std::min(max_w, best_w + coarse_w_step * 2);
    const int fine_h_min = std::max(min_h, best_h - coarse_h_step * 2);
    const int fine_h_max = std::min(max_h, best_h + coarse_h_step * 2);
    for (int w = fine_w_min; w <= fine_w_max; ++w) {
        for (int h = fine_h_min; h <= fine_h_max; ++h) {
            consider(w, h);
        }
    }

    if (!std::isfinite(best_score)) {
        result_label_->setText(QStringLiteral("自动校准没有得到有效匹配分数。"));
        return;
    }

    {
        QSignalBlocker bw(width_slider_);
        QSignalBlocker bh(height_slider_);
        width_slider_->setValue(std::clamp(static_cast<int>(std::lround(best_w * 100.0 / std::max(1, base_width_))), kScaleMin, kScaleMax));
        height_slider_->setValue(std::clamp(static_cast<int>(std::lround(best_h * 100.0 / std::max(1, base_height_))), kScaleMin, kScaleMax));
    }
    width_edit_->setText(QString::number(best_w));
    height_edit_->setText(QString::number(best_h));
    width_scale_label_->setText(QString::number(width_slider_->value()) + "%");
    height_scale_label_->setText(QString::number(height_slider_->value()) + "%");
    updatePreview();
    result_label_->setText(QStringLiteral("自动校准完成：%1 x %2，匹配分数 %3")
        .arg(best_w)
        .arg(best_h)
        .arg(best_score, 0, 'f', 3));
}

void ScaleCalibrationWindow::reloadTemplates() {
    const auto root = templateRootForKey(config_.paths().templatesDir(), currentRegionKey());
    template_paths_ = templateFiles(root);
    {
        QSignalBlocker blocker(template_combo_);
        template_combo_->clear();
        for (const auto& file : template_paths_) {
            template_combo_->addItem(templateDisplayName(file, root));
        }
        if (template_paths_.empty()) {
            template_combo_->addItem(QStringLiteral("未找到模板"));
        }
    }
    if (template_paths_.empty()) {
        current_template_path_.clear();
        template_outline_ = {};
        return;
    }
    current_template_path_ = template_paths_.front();
    {
        QSignalBlocker blocker(template_combo_);
        template_combo_->setCurrentIndex(0);
    }
    loadTemplatePreview();
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
    if (closing_ || updating_preview_) return;
    BoolGuard guard(updating_preview_);
    const std::string key = currentRegionKey();
    const int width = std::clamp(width_edit_->text().toInt(), 1, 2000);
    const int height = std::clamp(height_edit_->text().toInt(), 1, 2000);
    setPreviewPixmap(template_preview_, template_outline_, QStringLiteral("请选择模板"));
    setPreviewPixmap(capture_preview_, scaledCapturePixmap(key, width, height), QStringLiteral("无法截取当前区域"));
    const auto score = scaledMatchScore(key, width, height);
    if (score) {
        result_label_->setText(QStringLiteral("当前尺寸：%1 x %2，当前模板匹配分数：%3")
            .arg(width)
            .arg(height)
            .arg(*score, 0, 'f', 3));
    } else {
        result_label_->setText(QStringLiteral("当前尺寸：%1 x %2，暂无有效匹配分数").arg(width).arg(height));
    }
}

void ScaleCalibrationWindow::loadTemplatePreview() {
    template_outline_ = previewPixmapFromFile(current_template_path_, currentRegionKey());
}

void ScaleCalibrationWindow::stopRefresh() {
    if (refresh_timer_) {
        refresh_timer_->stop();
    }
}

void ScaleCalibrationWindow::startRefresh() {
    if (refresh_timer_ && !refresh_timer_->isActive()) {
        refresh_timer_->start();
    }
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
        return matToPixmap(previewForRegion(resized, key));
    } catch (const std::exception&) {
        return {};
    } catch (...) {
        return {};
    }
}

std::optional<double> ScaleCalibrationWindow::scaledMatchScore(const std::string& key, int width, int height) const {
    if (current_template_path_.empty()) return std::nullopt;
    try {
        const auto rect = regions_.getRealRegion(key);
        if (!rect || !rect->valid()) return std::nullopt;
        ScreenCapture capture;
        cv::Mat bgr = capture.grabBgr(*rect);
        if (bgr.empty()) return std::nullopt;
        cv::Mat resized;
        cv::resize(bgr, resized, cv::Size(width, height), 0.0, 0.0, cv::INTER_AREA);
        const cv::Mat tpl = cv::imread(current_template_path_.string(), cv::IMREAD_UNCHANGED);
        if (tpl.empty()) return std::nullopt;
        return matchScoreForRegion(resized, tpl, key);
    } catch (const std::exception&) {
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace pubg::ui
