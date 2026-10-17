#include "scenario_dialog.hpp"
#include "epoch_editor.hpp"
#include "workbench_style.hpp"
#include <QDateTime>
#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <orbitforge/runtime/time_frames.hpp>

namespace orbitforge::desktop {
std::optional<application::Scenario>
new_scenario_dialog(QWidget *parent, core::Epoch coverage_first, core::Epoch coverage_last,
                    const std::function<void(core::Epoch, core::Epoch)> &validate,
                    std::shared_ptr<const core::ITimeProvider> time) {
    QDialog dialog(parent);
    dialog.setObjectName("newScenarioDialog");
    dialog.setWindowTitle(QStringLiteral("新建场景 · Scenario"));
    dialog.setWindowIcon(workbench_icon(WorkbenchIcon::orbit));
    dialog.setMinimumWidth(560);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(theme::space::page);
    layout->setSpacing(16);
    auto *title = new QLabel(QStringLiteral("新建场景"));
    title->setObjectName("panelTitle");
    layout->addWidget(title);
    auto *description = new QLabel(QStringLiteral("为新的轨道分析设定名称与时间范围。"));
    description->setObjectName("dialogDescription");
    layout->addWidget(description);
    auto *form = new QFormLayout;
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(12);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto *name = new QLineEdit("Scenario1");
    name->setMaximumWidth(380);
    name->setObjectName("scenarioName");
    name->selectAll();
    form->addRow(QStringLiteral("场景名称"), name);
    form->addRow(QStringLiteral("中心天体"), new QLabel(QStringLiteral("Earth / 地球")));
    if (!time)
        time = runtime::make_builtin_time_provider();
    const QDate today = QDateTime::currentDateTimeUtc().date();
    const auto midnight = [&](QDate date) {
        return time->from_utc({date.year(), date.month(), date.day(), 0, 0, 0, 0});
    };
    const auto make_time = [&](core::Epoch value) {
        auto *edit = new EpochEditor(nullptr, time);
        edit->set_epoch(value);
        return edit;
    };
    auto *first = make_time(midnight(today));
    auto *last = make_time(midnight(today.addDays(1)));
    first->setObjectName("scenarioStart");
    last->setObjectName("scenarioEnd");
    form->addRow(QStringLiteral("开始时间"), first);
    form->addRow(QStringLiteral("结束时间"), last);
    auto *error = new QLabel;
    error->setObjectName("inputError");
    error->setWordWrap(true);
    error->hide();
    layout->addLayout(form);
    const auto coverage =
        QStringLiteral("可用时段：%1 — %2 UTC")
            .arg(calendar_text(coverage_first, CalendarScale::utc, true, time.get()),
                 calendar_text(coverage_last, CalendarScale::utc, true, time.get()));
    first->setToolTip(coverage);
    last->setToolTip(coverage);
    layout->addWidget(error);
    auto *divider = new QFrame;
    divider->setObjectName("sectionDivider");
    divider->setFixedHeight(1);
    layout->addWidget(divider);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *cancel = new QPushButton(QStringLiteral("取消"));
    cancel->setAutoDefault(false);
    cancel->setObjectName("cancelScenario");
    auto *create = new QPushButton(QStringLiteral("创建场景"));
    create->setDefault(true);
    create->setObjectName("applyOrbit");
    buttons->addWidget(cancel);
    buttons->addWidget(create);
    layout->addLayout(buttons);
    std::optional<application::Scenario> result;
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(create, &QPushButton::clicked, &dialog, [&] {
        try {
            validate(first->epoch(), last->epoch());
            result.emplace(name->text().trimmed().toStdString(), first->epoch(), last->epoch());
            dialog.accept();
        } catch (const std::exception &exception) {
            error->setText(QString::fromUtf8(exception.what()));
            error->show();
        }
    });
    if (dialog.exec() != QDialog::Accepted)
        result.reset();
    return result;
}
} // namespace orbitforge::desktop
