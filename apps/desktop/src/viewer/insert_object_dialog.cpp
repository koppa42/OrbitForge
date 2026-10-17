#include "insert_object_dialog.hpp"
#include "workbench_style.hpp"
#include <QButtonGroup>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace orbitforge::desktop {
std::optional<InsertObjectRequest> insert_object_dialog(QWidget *parent,
                                                        const QString &scenario_name) {
    QDialog dialog(parent);
    dialog.setObjectName("insertObjectsDialog");
    dialog.setWindowTitle(QStringLiteral("插入对象"));
    dialog.setWindowIcon(workbench_icon(WorkbenchIcon::satellite));
    dialog.setMinimumSize(540, 280);
    const auto *screen = parent ? parent->screen() : dialog.screen();
    const auto available = screen->availableGeometry().size() - QSize(48, 80);
    dialog.resize(QSize(780, 440).boundedTo(available).expandedTo(dialog.minimumSize()));
    dialog.setSizeGripEnabled(true);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(theme::space::page);
    layout->setSpacing(16);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("插入对象"));
    title->setObjectName("panelTitle");
    heading->addWidget(title);
    heading->addStretch();
    auto *destination = new QLabel;
    destination->setObjectName("muted");
    destination->setTextFormat(Qt::PlainText);
    destination->setText(destination->fontMetrics().elidedText(
        QStringLiteral("场景 / ") + scenario_name, Qt::ElideMiddle, 240));
    destination->setToolTip(scenario_name);
    heading->addWidget(destination);
    layout->addLayout(heading);
    auto *description = new QLabel(QStringLiteral("选择对象类型，再设置初始参数。"));
    description->setObjectName("dialogDescription");
    layout->addWidget(description);

    auto *columns = new QHBoxLayout;
    columns->setSpacing(20);
    auto *left = new QVBoxLayout;
    left->setSpacing(6);
    auto *type_heading = new QLabel(QStringLiteral("对象类型"));
    type_heading->setObjectName("sectionTitle");
    left->addWidget(type_heading);
    auto *types = new QTreeWidget;
    types->setObjectName("insertObjectTypes");
    types->setAccessibleName(QStringLiteral("对象类型"));
    types->setHeaderHidden(true);
    types->setRootIsDecorated(false);
    types->setItemsExpandable(false);
    types->setIndentation(14);
    types->setIconSize({16, 16});
    types->setMinimumWidth(160);
    auto *group = new QTreeWidgetItem(types, {QStringLiteral("场景对象")});
    group->setFlags(Qt::ItemIsEnabled);
    group->setForeground(0, theme::color(theme::text_muted));
    auto *satellite = new QTreeWidgetItem(group, {QStringLiteral("卫星")});
    satellite->setIcon(0, workbench_icon(WorkbenchIcon::satellite, theme::color(theme::accent)));
    satellite->setData(0, Qt::UserRole, static_cast<int>(InsertObjectType::satellite));
    auto *station = new QTreeWidgetItem(group, {QStringLiteral("地面站")});
    station->setIcon(0,
                     workbench_icon(WorkbenchIcon::ground_station, theme::color(theme::success)));
    station->setData(0, Qt::UserRole, static_cast<int>(InsertObjectType::ground_station));
    group->setExpanded(true);
    types->setCurrentItem(satellite);
    left->addWidget(types, 1);
    columns->addLayout(left, 2);

    auto *right = new QVBoxLayout;
    right->setSpacing(0);
    auto *method_heading = new QLabel(QStringLiteral("插入方式"));
    method_heading->setObjectName("sectionTitle");
    right->addWidget(method_heading);
    right->addSpacing(10);
    auto *methods = new QButtonGroup(&dialog);
    const auto add_method = [&](const char *name, InsertObjectMethod method, const QString &text,
                                const QString &description) {
        auto *choice = new QRadioButton(text);
        choice->setObjectName(name);
        choice->setAccessibleDescription(description);
        methods->addButton(choice, static_cast<int>(method));
        auto *card = new QFrame;
        card->setObjectName("choiceCard");
        card->setProperty("selected", false);
        auto *card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(theme::space::card);
        card_layout->setSpacing(2);
        card_layout->addWidget(choice);
        auto *caption = new QLabel(description);
        caption->setProperty("method", static_cast<int>(method));
        caption->setObjectName("hint");
        caption->setContentsMargins(22, 4, 0, 0);
        caption->setWordWrap(true);
        card_layout->addWidget(caption);
        right->addWidget(card);
        QObject::connect(choice, &QRadioButton::toggled, card, [card](bool selected) {
            card->setProperty("selected", selected);
            card->style()->unpolish(card);
            card->style()->polish(card);
            card->update();
        });
        return choice;
    };
    auto *properties =
        add_method("insertDefineProperties", InsertObjectMethod::define_properties,
                   QStringLiteral("定义属性"), QStringLiteral("设置轨道、参考系与传播器"));
    properties->setChecked(true);
    right->addSpacing(10);
    add_method("insertDefaults", InsertObjectMethod::defaults, QStringLiteral("使用默认参数"),
               QStringLiteral("二体传播 · 场景起始时刻 · 60 s 步长"));
    right->addStretch(1);
    columns->addLayout(right, 3);
    layout->addLayout(columns, 1);

    auto *divider = new QFrame;
    divider->setObjectName("sectionDivider");
    divider->setFixedHeight(1);
    layout->addWidget(divider);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(10);
    buttons->addStretch();
    auto *close = new QPushButton(QStringLiteral("关闭"));
    close->setObjectName("closeInsertObjects");
    close->setAutoDefault(false);
    auto *insert = new QPushButton(QStringLiteral("配置属性"));
    insert->setObjectName("insertObject");
    insert->setDefault(true);
    insert->setMinimumWidth(84);
    buttons->addWidget(close);
    buttons->addWidget(insert);
    layout->addLayout(buttons);
    QObject::connect(methods, &QButtonGroup::idToggled, &dialog, [insert](int id, bool checked) {
        if (checked)
            insert->setText(id == static_cast<int>(InsertObjectMethod::define_properties)
                                ? QStringLiteral("配置属性")
                                : QStringLiteral("直接插入"));
    });
    QObject::connect(
        types, &QTreeWidget::currentItemChanged, &dialog,
        [insert, &dialog, methods](QTreeWidgetItem *item) {
            insert->setEnabled(item && item->data(0, Qt::UserRole).isValid());
            const bool station_type =
                item && item->data(0, Qt::UserRole).toInt() ==
                            static_cast<int>(InsertObjectType::ground_station);
            for (auto *label : dialog.findChildren<QLabel *>()) {
                if (!label->property("method").isValid())
                    continue;
                const auto method = label->property("method").toInt();
                const auto caption =
                    method == static_cast<int>(InsertObjectMethod::defaults)
                        ? (station_type ? QStringLiteral("WGS84 · 0° 经度 · 0° 纬度 · 0 m 椭球高")
                                        : QStringLiteral("二体传播 · 场景起始时刻 · 60 s 步长"))
                        : (station_type ? QStringLiteral("设置名称、经纬度与椭球高")
                                        : QStringLiteral("设置轨道、参考系与传播器"));
                label->setText(caption);
                methods->button(method)->setAccessibleDescription(caption);
            }
        });
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    std::optional<InsertObjectRequest> result;
    QObject::connect(insert, &QPushButton::clicked, &dialog, [&] {
        const auto *item = types->currentItem();
        if (!item || !item->data(0, Qt::UserRole).isValid() || methods->checkedId() < 0)
            return;
        result =
            InsertObjectRequest{static_cast<InsertObjectType>(item->data(0, Qt::UserRole).toInt()),
                                static_cast<InsertObjectMethod>(methods->checkedId())};
        dialog.accept();
    });
    types->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        result.reset();
    return result;
}
} // namespace orbitforge::desktop
