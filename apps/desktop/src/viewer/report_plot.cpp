#include "report_plot.hpp"
#include "workbench_theme.hpp"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>

namespace orbitforge::desktop {
namespace {
QString str(const std::string &text) { return QString::fromStdString(text); }
double nice_step(double value) {
    const auto scale = std::pow(10.0, std::floor(std::log10(value)));
    for (auto multiplier : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (value <= multiplier * scale)
            return multiplier * scale;
    return 10 * scale;
}
QString axis_number(double value, double step) {
    if (std::abs(value) < step * 1e-8)
        value = 0;
    if (std::abs(value) >= 1e9 || (step < 1e-6 && value != 0))
        return QString::number(value, 'g', 9);
    const auto decimals = std::clamp(static_cast<int>(-std::floor(std::log10(step))) + 1, 0, 10);
    auto text = QString::number(value, 'f', decimals);
    if (text.contains('.')) {
        while (text.endsWith('0'))
            text.chop(1);
        if (text.endsWith('.'))
            text.chop(1);
    }
    return text;
}
constexpr const char *colors[]{"#a9a7ff", "#6cdaca", "#ffbd87", "#87bcff", "#efa0cc", "#bcd68a",
                               "#dac5ff", "#82d0ee", "#e9d67f", "#e99595", "#97adb9", "#cfb49a"};
} // namespace
ReportPlot::ReportPlot(QWidget *parent) : QWidget(parent) {
    setObjectName("reportPlot");
    setAccessibleName(QStringLiteral("报告时间曲线"));
    setMinimumSize(0, 220);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}
void ReportPlot::set_result(std::shared_ptr<const runtime::ReportResult> result, std::size_t column,
                            QString label, std::string object) {
    result_ = std::move(result);
    column_ = column;
    label_ = std::move(label);
    object_ = std::move(object);
    rebuild();
}
void ReportPlot::set_field(std::size_t column, QString label) {
    column_ = column;
    label_ = std::move(label);
    rebuild();
}
void ReportPlot::set_object(std::string id) {
    object_ = std::move(id);
    rebuild();
}
void ReportPlot::rebuild() {
    series_.clear();
    hover_ = dragging_ = false;
    if (!result_ || column_ >= result_->table.columns().size()) {
        update();
        return;
    }
    const auto &columns = result_->table.columns();
    const auto time = std::find_if(columns.begin(), columns.end(),
                                   [](const auto &c) { return c.id == "time.epoch"; });
    const auto id_column = std::ranges::find(columns, "object.id", &results::Column::id);
    const auto name_column = std::ranges::find(columns, "object.name", &results::Column::id);
    if (time == columns.end() || id_column == columns.end() || name_column == columns.end()) {
        update();
        return;
    }
    time_column_ = static_cast<std::size_t>(time - columns.begin());
    const auto &field = columns[column_].id;
    const bool cyclic = field == "geodetic.longitude" || field == "elements.raan" ||
                        field == "elements.argp" || field == "elements.nu" ||
                        field == "aer.azimuth";
    const auto half_turn = result_->column_unit(column_) == "deg" ? 180.0 : std::numbers::pi;
    std::map<std::string, std::size_t> indices;
    std::map<std::string, std::size_t> color_indices;
    std::set<std::string> missing;
    for (std::size_t r = 0; r < result_->table.rows().size(); ++r) {
        const auto &row = result_->table.rows()[r];
        const auto *id = std::get_if<std::string>(&row[id_column - columns.begin()]);
        const auto *epoch = std::get_if<core::Epoch>(&row[time_column_]);
        const auto value = result_->numeric_value(r, column_);
        if (!id || !epoch)
            continue;
        if (!value) {
            missing.insert(*id);
            continue;
        }
        const auto color = color_indices.try_emplace(*id, color_indices.size()).first->second;
        if (!object_.empty() && object_ != *id)
            continue;
        const auto [entry, inserted] = indices.try_emplace(*id, series_.size());
        if (inserted) {
            auto label = str(std::get<std::string>(row[name_column - columns.begin()]));
            // Stable IDs disambiguate objects with identical names in the legend and hover text.
            const bool duplicate = std::any_of(series_.begin(), series_.end(),
                                               [&](const auto &s) { return s.label == label; });
            if (duplicate)
                label += " [" + str(*id) + "]";
            series_.push_back({label, QColor(colors[color % std::size(colors)]), {}, {}});
        }
        auto &points = series_[entry->second].points;
        const bool gap =
            missing.erase(*id) != 0 ||
            (cyclic && !points.empty() && std::abs(points.back().y - *value) > half_turn);
        points.push_back({epoch->seconds_since(result_->first), *value, r, gap});
    }
    full_left_ = 0;
    full_right_ = result_->last.seconds_since(result_->first);
    if (full_right_ == 0) {
        full_left_ = -0.5;
        full_right_ = 0.5;
    }
    reset_view();
}
void ReportPlot::reset_view() { set_interval(full_left_, full_right_); }
void ReportPlot::set_interval(double left, double right) {
    const auto min_span = std::max(1e-9, (full_right_ - full_left_) * 1e-9);
    if (right - left < min_span)
        return;
    const auto span = std::min(right - left, full_right_ - full_left_);
    left_ = std::clamp(left, full_left_, full_right_ - span);
    right_ = left_ + span;
    layout_plot();
    update();
}
QPointF ReportPlot::pixel(const Point &point) const {
    return {plot_.left() + (point.x - left_) / (right_ - left_) * plot_.width(),
            plot_.bottom() - (point.y - low_) / (high_ - low_) * plot_.height()};
}
double ReportPlot::time_at(double x) const {
    return left_ + std::clamp((x - plot_.left()) / plot_.width(), 0.0, 1.0) * (right_ - left_);
}
void ReportPlot::layout_plot() {
    const auto fm = fontMetrics();
    const auto per_row = std::max(1, (width() - 32) / 180);
    const auto legend_rows =
        std::min(2, (static_cast<int>(series_.size()) + per_row - 1) / per_row);
    const double top = 48 + legend_rows * (fm.height() + 6);
    low_ = std::numeric_limits<double>::infinity();
    high_ = -low_;
    for (const auto &series : series_) {
        // Include the two neighbouring samples so a zoom between samples remains meaningful.
        auto begin = std::lower_bound(series.points.begin(), series.points.end(), left_,
                                      [](const auto &p, double x) { return p.x < x; });
        if (begin != series.points.begin())
            --begin;
        const auto end = std::upper_bound(series.points.begin(), series.points.end(), right_,
                                          [](double x, const auto &p) { return x < p.x; });
        auto extended = end;
        if (extended != series.points.end())
            ++extended;
        for (auto it = begin; it != extended; ++it) {
            low_ = std::min(low_, it->y);
            high_ = std::max(high_, it->y);
        }
    }
    if (!std::isfinite(low_)) {
        low_ = 0;
        high_ = 1;
    }
    const auto padding = std::max((high_ - low_) * 0.06, std::max(std::abs(low_), 1.0) * 1e-8);
    low_ -= padding;
    high_ += padding;
    y_step_ = nice_step((high_ - low_) / 4);
    low_ = std::floor(low_ / y_step_) * y_step_;
    high_ = std::ceil(high_ / y_step_) * y_step_;
    int axis_width = 48;
    const auto y_ticks = static_cast<int>(std::round((high_ - low_) / y_step_));
    for (int i = 0; i <= y_ticks; ++i)
        axis_width = std::max(axis_width,
                              fm.horizontalAdvance(axis_number(low_ + y_step_ * i, y_step_)) + 18);
    axis_width = std::min(axis_width, std::max(48, width() / 3));
    plot_ = QRectF(axis_width, top, std::max(1, width() - axis_width - 22),
                   std::max(1.0, height() - top - 76));
    // Keep first, extrema and last per horizontal pixel bucket, in chronological order.
    // This bounds painted vertices while retaining narrow peaks; raw rows remain untouched.
    for (auto &series : series_) {
        series.path = {};
        std::vector<const Point *> bucket;
        int previous_pixel = std::numeric_limits<int>::min();
        bool connected = false;
        const auto flush = [&] {
            if (bucket.empty())
                return;
            const auto mm = std::minmax_element(bucket.begin(), bucket.end(),
                                                [](auto a, auto b) { return a->y < b->y; });
            std::array<const Point *, 4> keep{bucket.front(), *mm.first, *mm.second, bucket.back()};
            std::sort(keep.begin(), keep.end(), [](auto a, auto b) { return a->row < b->row; });
            const Point *previous = nullptr;
            for (auto p : keep) {
                if (p == previous)
                    continue;
                if (!connected) {
                    series.path.moveTo(pixel(*p));
                    connected = true;
                } else
                    series.path.lineTo(pixel(*p));
                previous = p;
            }
            bucket.clear();
        };
        auto begin = std::lower_bound(series.points.begin(), series.points.end(), left_,
                                      [](const auto &p, double x) { return p.x < x; });
        if (begin != series.points.begin())
            --begin;
        for (auto it = begin; it != series.points.end(); ++it) {
            const auto px = static_cast<int>(std::clamp(pixel(*it).x(), -1e6, 1e6));
            if (px != previous_pixel || it->gap) {
                flush();
                previous_pixel = px;
            }
            if (it->gap)
                connected = false;
            bucket.push_back(&*it);
            if (it->x > right_)
                break;
        }
        flush();
    }
}
void ReportPlot::draw(QPainter &p, bool interaction) const {
    p.fillRect(rect(), theme::color(theme::chrome));
    p.setRenderHint(QPainter::Antialiasing);
    p.setFont(font());
    p.setPen(theme::color(theme::text));
    const auto fm = fontMetrics();
    p.drawText(QRect(16, 10, width() - 32, 28), Qt::AlignLeft | Qt::AlignVCenter,
               fm.elidedText(label_, Qt::ElideRight, width() - 32));
    if (series_.empty()) {
        p.setPen(theme::color(theme::text_muted));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("此结果没有可绘制的时间序列"));
        return;
    }
    const auto per_row = std::max(1, (width() - 32) / 180);
    const auto legend_count = std::min(series_.size(), static_cast<std::size_t>(per_row * 2));
    for (std::size_t i = 0; i < legend_count; ++i) {
        const auto x = 16 + static_cast<int>(i) % per_row * 180;
        const auto y = 46 + static_cast<int>(i) / per_row * (fm.height() + 6);
        p.setPen(QPen(series_[i].color, 2));
        p.drawLine(x, y + fm.height() / 2, x + 16, y + fm.height() / 2);
        p.setPen(theme::color(theme::text_soft));
        p.drawText(QRect(x + 24, y, 148, fm.height()), Qt::AlignVCenter,
                   i + 1 == legend_count && legend_count < series_.size()
                       ? QStringLiteral("另 %1 个对象…").arg(series_.size() - legend_count + 1)
                       : fm.elidedText(series_[i].label, Qt::ElideRight, 148));
    }
    const auto y_ticks = static_cast<int>(std::round((high_ - low_) / y_step_));
    for (int i = 0; i <= y_ticks; ++i) {
        const auto y = plot_.bottom() - plot_.height() * i / y_ticks;
        p.setPen(QPen(theme::color(theme::line), 1));
        p.drawLine(QPointF(plot_.left(), y), QPointF(plot_.right(), y));
        p.setPen(theme::color(theme::text_muted));
        p.drawText(QRectF(2, y - fm.height() / 2.0, plot_.left() - 10, fm.height()),
                   Qt::AlignRight | Qt::AlignVCenter, axis_number(low_ + y_step_ * i, y_step_));
    }
    const double time_scale = right_ - left_ >= 7200 ? 3600 : right_ - left_ >= 120 ? 60 : 1;
    const auto x_step = nice_step((right_ - left_) / time_scale / (width() < 500 ? 3 : 5));
    const auto first_tick = std::ceil(left_ / time_scale / x_step) * x_step;
    for (int i = 0; i < 10; ++i) {
        const auto value = first_tick + i * x_step;
        if (value > right_ / time_scale + x_step * 1e-9)
            break;
        const auto x =
            plot_.left() + plot_.width() * (value * time_scale - left_) / (right_ - left_);
        p.setPen(QPen(theme::color(theme::line, 150), 1));
        p.drawLine(QPointF(x, plot_.top()), QPointF(x, plot_.bottom()));
        p.setPen(theme::color(theme::text_muted));
        const auto align = x - plot_.left() < 30    ? Qt::AlignLeft
                           : plot_.right() - x < 30 ? Qt::AlignRight
                                                    : Qt::AlignHCenter;
        const auto start = align == Qt::AlignLeft ? x : align == Qt::AlignRight ? x - 100 : x - 50;
        p.drawText(QRectF(start, plot_.bottom() + 8, 100, fm.height()), align | Qt::AlignVCenter,
                   axis_number(value, x_step));
    }
    auto origin = str(result_->cell_text(series_.front().points.front().row, time_column_));
    if (origin.endsWith('Z'))
        origin.chop(1);
    while (origin.endsWith('0'))
        origin.chop(1);
    if (origin.endsWith('.'))
        origin.chop(1);
    origin.replace('T', ' ');
    // Every report series starts at result.first, including when filtering by object.
    const auto axis = QStringLiteral("自 %1 %2 起 / %3")
                          .arg(origin, str(result_->column_unit(time_column_)),
                               time_scale == 3600 ? "h"
                               : time_scale == 60 ? "min"
                                                  : "s");
    p.drawText(QRectF(12, height() - 42, width() - 24, 34), Qt::AlignCenter | Qt::TextWordWrap,
               axis);
    p.save();
    p.setClipRect(plot_.adjusted(-2, -2, 2, 2));
    for (const auto &series : series_) {
        p.setPen(QPen(series.color, 1.8));
        p.setBrush(Qt::NoBrush);
        p.drawPath(series.path);
        if (series.points.size() <= 80) {
            p.setBrush(series.color);
            for (const auto &point : series.points)
                p.drawEllipse(pixel(point), 2.4, 2.4);
        }
    }
    if (interaction && hover_) {
        if (const auto *point = nearest(cursor_)) {
            const auto pos = pixel(*point);
            p.setPen(QPen(theme::color(theme::text_muted), 1, Qt::DashLine));
            p.drawLine(QPointF(pos.x(), plot_.top()), QPointF(pos.x(), plot_.bottom()));
            p.setBrush(theme::color(theme::text));
            p.drawEllipse(pos, 4, 4);
        }
    }
    if (interaction && dragging_) {
        p.fillRect(QRectF(QPointF(std::min(drag_start_.x(), cursor_.x()), plot_.top()),
                          QPointF(std::max(drag_start_.x(), cursor_.x()), plot_.bottom())),
                   theme::color(theme::accent, 50));
    }
    p.restore();
}
QImage ReportPlot::image(qreal scale) const {
    scale = std::clamp(scale, 1.0, 4.0);
    QImage output(QSize(qRound(width() * scale), qRound(height() * scale)),
                  QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    painter.scale(scale, scale);
    draw(painter, false);
    return output;
}
void ReportPlot::paintEvent(QPaintEvent *) {
    QPainter p(this);
    draw(p, true);
}
void ReportPlot::resizeEvent(QResizeEvent *) { layout_plot(); }
const ReportPlot::Point *ReportPlot::nearest(QPointF position, const Series **source) const {
    const Point *best = nullptr;
    double distance = std::numeric_limits<double>::infinity();
    const auto epoch = time_at(position.x());
    for (const auto &series : series_) {
        auto it = std::lower_bound(series.points.begin(), series.points.end(), epoch,
                                   [](const auto &p, double x) { return p.x < x; });
        for (int i = 0; i < 2; ++i) {
            if (it != series.points.end()) {
                const auto d = pixel(*it) - position;
                const auto length = d.x() * d.x() + d.y() * d.y();
                if (length < distance) {
                    distance = length;
                    best = &*it;
                    if (source)
                        *source = &series;
                }
            }
            if (it == series.points.begin())
                break;
            --it;
        }
    }
    return best;
}
void ReportPlot::mouseMoveEvent(QMouseEvent *event) {
    cursor_ = event->position();
    hover_ = plot_.contains(cursor_);
    const Series *series = nullptr;
    if (hover_ && !dragging_) {
        if (const auto *point = nearest(cursor_, &series)) {
            QToolTip::showText(
                event->globalPosition().toPoint(),
                "<b>" + series->label.toHtmlEscaped() + "</b><br>" +
                    str(result_->cell_text(point->row, time_column_)).toHtmlEscaped() + " " +
                    str(result_->column_unit(time_column_)) + "<br>" + label_.toHtmlEscaped() +
                    ": " + str(result_->cell_text(point->row, column_)),
                this);
        }
    } else
        QToolTip::hideText();
    update();
}
void ReportPlot::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::RightButton) {
        reset_view();
        return;
    }
    if (event->button() == Qt::LeftButton && plot_.contains(event->position())) {
        drag_start_ = cursor_ = event->position();
        dragging_ = true;
        setFocus();
    }
}
void ReportPlot::mouseReleaseEvent(QMouseEvent *event) {
    if (!dragging_ || event->button() != Qt::LeftButton)
        return;
    dragging_ = false;
    cursor_ = event->position();
    if (std::abs(cursor_.x() - drag_start_.x()) > 8)
        set_interval(time_at(std::min(cursor_.x(), drag_start_.x())),
                     time_at(std::max(cursor_.x(), drag_start_.x())));
    update();
}
void ReportPlot::mouseDoubleClickEvent(QMouseEvent *event) {
    dragging_ = false;
    if (event->button() == Qt::LeftButton && plot_.contains(event->position()) && seek)
        if (const auto *point = nearest(event->position()))
            seek(std::get<core::Epoch>(result_->table.rows()[point->row][time_column_]));
}
void ReportPlot::wheelEvent(QWheelEvent *event) {
    if (!plot_.contains(event->position()) || series_.empty()) {
        event->ignore();
        return;
    }
    const auto anchor = time_at(event->position().x());
    const auto factor = std::pow(1.25, -event->angleDelta().y() / 120.0);
    set_interval(anchor + (left_ - anchor) * factor, anchor + (right_ - anchor) * factor);
    event->accept();
}
void ReportPlot::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Home || event->key() == Qt::Key_Escape)
        reset_view();
    else if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right) {
        const auto shift = (right_ - left_) * 0.1 * (event->key() == Qt::Key_Left ? -1 : 1);
        set_interval(left_ + shift, right_ + shift);
    } else
        QWidget::keyPressEvent(event);
}
void ReportPlot::leaveEvent(QEvent *) {
    hover_ = false;
    QToolTip::hideText();
    update();
}
} // namespace orbitforge::desktop
