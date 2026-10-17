#pragma once
#include "workbench_theme.hpp"
#include <QColor>
#include <QIcon>
#include <QPixmap>
class QApplication;
class QDockWidget;
class QPainter;
class QStyleOptionViewItem;
class QRectF;
namespace orbitforge::desktop {
enum class WorkbenchIcon {
    new_scenario,
    folder,
    save,
    download,
    database,
    orbit,
    earth,
    map,
    satellite,
    ground_station,
    properties,
    fit,
    focus,
    views,
    play,
    pause,
    first,
    last,
    slower,
    faster,
    search,
    close,
    detach,
    calendar,
    clock,
    zoom_in,
    zoom_out,
    time_fit,
    chevron_up,
    chevron_down,
    add,
    remove,
    info,
    chevron_right
};
void apply_workbench_style(QApplication &app);
QIcon workbench_icon(WorkbenchIcon icon, const QColor &color = theme::color(theme::text_soft));
void style_dock(QDockWidget *dock, WorkbenchIcon icon);
// Application mark: the orbit glyph on a rounded ice-to-sakura tile, in logical pixels.
QPixmap brand_mark(int size, qreal device_pixel_ratio = 1.0);
// Shared list-row chrome for painted item delegates: a soft wash on hover, and the selected
// row marked by an accent wash with a leading stripe (in `stripe`); focus adds a faint outline.
void paint_list_row(QPainter *painter, const QStyleOptionViewItem &option, const QRectF &row,
                    const QColor &stripe);
} // namespace orbitforge::desktop
