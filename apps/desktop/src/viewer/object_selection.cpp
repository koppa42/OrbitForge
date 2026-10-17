#include "object_selection.hpp"
#include "workbench_style.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace orbitforge::desktop {
namespace {
constexpr int id_role = Qt::UserRole, kind_role = Qt::UserRole + 1;
constexpr int path_role = Qt::UserRole + 2, available_role = Qt::UserRole + 3;
QString object_text(const std::string &s) { return QString::fromStdString(s); }
QString kind_name(ObjectKind kind) {
    switch (kind) {
    case ObjectKind::satellite:
        return QStringLiteral("卫星");
    case ObjectKind::ground_station:
        return QStringLiteral("地面站");
    case ObjectKind::sensor:
        return QStringLiteral("传感器");
    default:
        return QStringLiteral("对象");
    }
}
QIcon object_icon(ObjectKind kind) {
    return workbench_icon(kind == ObjectKind::satellite        ? WorkbenchIcon::satellite
                          : kind == ObjectKind::ground_station ? WorkbenchIcon::ground_station
                          : kind == ObjectKind::sensor         ? WorkbenchIcon::focus
                                                               : WorkbenchIcon::properties);
}
bool contains(const std::vector<std::string> &ids, const std::string &id) {
    return std::ranges::find(ids, id) != ids.end();
}
} // namespace

std::vector<ObjectChoice> project_object_choices(const application::Project &project,
                                                 std::initializer_list<ObjectKind> allowed) {
    const auto permits = [&](ObjectKind kind) {
        return std::ranges::find(allowed, kind) != allowed.end();
    };
    std::vector<ObjectChoice> objects;
    std::unordered_map<std::string, QString> parents;
    std::unordered_set<std::string> sensor_parents;
    if (permits(ObjectKind::sensor))
        for (const auto &sensor : project.sensors)
            sensor_parents.insert(sensor.parent_satellite_id);
    for (const auto &satellite : project.satellites) {
        const auto &id = satellite.orbit.object_id;
        parents.emplace(id, object_text(satellite.name()));
        if (permits(ObjectKind::satellite) || sensor_parents.contains(id))
            objects.push_back({id,
                               object_text(satellite.name()),
                               ObjectKind::satellite,
                               {},
                               object_text(satellite.name()),
                               permits(ObjectKind::satellite)});
    }
    if (permits(ObjectKind::ground_station))
        for (const auto &station : project.ground_stations)
            objects.push_back({station.object_id,
                               object_text(station.name()),
                               ObjectKind::ground_station,
                               {},
                               object_text(station.name())});
    if (permits(ObjectKind::sensor))
        for (const auto &sensor : project.sensors) {
            const auto parent = parents.find(sensor.parent_satellite_id);
            const auto parent_name =
                parent == parents.end() ? object_text(sensor.parent_satellite_id) : parent->second;
            objects.push_back({sensor.object_id, object_text(sensor.name()), ObjectKind::sensor,
                               sensor.parent_satellite_id,
                               parent_name + QStringLiteral(" / ") + object_text(sensor.name())});
        }
    return objects;
}

ObjectSelectionDialog::ObjectSelectionDialog(std::vector<ObjectChoice> objects,
                                             ObjectSelectionOptions options,
                                             ObjectSelection selection, QWidget *parent)
    : QDialog(parent), options_(std::move(options)), value_(std::move(selection)) {
    setObjectName("objectSelectionDialog");
    setWindowTitle(QStringLiteral("对象选择窗口"));
    setWindowIcon(workbench_icon(WorkbenchIcon::search));
    resize(600, 540);
    setMinimumSize(360, 360);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(theme::space::page);
    layout->setSpacing(12);
    auto *title = new QLabel(QStringLiteral("选择对象"), this);
    title->setObjectName("panelTitle");
    layout->addWidget(title);
    auto *purpose = new QLabel(options_.purpose, this);
    purpose->setObjectName("objectSelectionPurpose");
    purpose->setTextFormat(Qt::PlainText);
    purpose->setWordWrap(true);
    layout->addWidget(purpose);
    auto *search_row = new QHBoxLayout;
    search_ = new QLineEdit(this);
    search_->setObjectName("objectSelectionSearch");
    search_->setPlaceholderText(QStringLiteral("搜索名称、路径或 ID…"));
    search_->setAccessibleName(QStringLiteral("搜索对象"));
    search_->setClearButtonEnabled(true);
    search_->addAction(workbench_icon(WorkbenchIcon::search), QLineEdit::LeadingPosition);
    search_row->addWidget(search_, 1);
    kind_ = new QComboBox(this);
    kind_->setObjectName("objectSelectionKind");
    kind_->setAccessibleName(QStringLiteral("对象类型筛选"));
    search_row->addWidget(kind_);
    layout->addLayout(search_row);
    all_ = new QCheckBox(options_.all_label, this);
    all_->setObjectName("objectSelectionAll");
    all_->setChecked(value_.all && !options_.all_label.isEmpty());
    value_.all = all_->isChecked();
    all_->setVisible(!options_.all_label.isEmpty());
    layout->addWidget(all_);
    tree_ = new QTreeWidget(this);
    tree_->setObjectName("objectSelectionTree");
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({QStringLiteral("对象"), QStringLiteral("类型")});
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setTextElideMode(Qt::ElideMiddle);
    tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    tree_->setColumnWidth(1, 76);
    tree_->setEnabled(!value_.all);
    layout->addWidget(tree_, 1);
    auto *empty_layout = new QVBoxLayout(tree_->viewport());
    empty_layout->addStretch();
    empty_ = new QLabel(tree_->viewport());
    empty_->setObjectName("objectSelectionEmpty");
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    empty_->setAttribute(Qt::WA_TransparentForMouseEvents);
    empty_layout->addWidget(empty_);
    empty_layout->addStretch();
    details_ = new QLabel(this);
    details_->setObjectName("objectSelectionDetails");
    details_->setTextFormat(Qt::PlainText);
    details_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    details_->setFixedHeight(38);
    layout->addWidget(details_);
    auto *footer = new QHBoxLayout;
    count_ = new QLabel(this);
    count_->setObjectName("objectSelectionCount");
    count_->setWordWrap(true);
    footer->addWidget(count_, 1);
    clear_ = new QPushButton(QStringLiteral("清空"), this);
    clear_->setObjectName("objectSelectionClear");
    clear_->setAutoDefault(false);
    clear_->setVisible(options_.multiple);
    footer->addWidget(clear_);
    layout->addLayout(footer);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    confirm_ = buttons->button(QDialogButtonBox::Ok);
    confirm_->setText(QStringLiteral("确定"));
    confirm_->setProperty("primary", true);
    confirm_->style()->unpolish(confirm_);
    confirm_->style()->polish(confirm_);
    confirm_->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &ObjectSelectionDialog::confirm);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(search_, &QLineEdit::textChanged, this, [this] { filter(); });
    connect(kind_, &QComboBox::currentIndexChanged, this, [this] { filter(); });
    connect(all_, &QCheckBox::toggled, this, [this](bool checked) {
        value_.all = checked;
        tree_->setEnabled(!checked);
        update_status();
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (rebuilding_)
            return;
        if (!options_.multiple && item && item->data(0, available_role).toBool())
            value_.ids = {item->data(0, id_role).toString().toStdString()};
        update_status();
    });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int) {
        if (rebuilding_ || !options_.multiple)
            return;
        const auto id = item->data(0, id_role).toString().toStdString();
        std::erase(value_.ids, id);
        if (item->checkState(0) == Qt::Checked)
            value_.ids.push_back(id);
        update_status();
    });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (!options_.multiple && item->data(0, available_role).toBool())
            confirm();
    });
    connect(clear_, &QPushButton::clicked, this, [this] {
        const QSignalBlocker guard(tree_);
        value_.ids.clear();
        for (QTreeWidgetItemIterator it(tree_); *it; ++it)
            if ((*it)->flags().testFlag(Qt::ItemIsUserCheckable))
                (*it)->setCheckState(0, Qt::Unchecked);
        update_status();
    });
    set_objects(std::move(objects));
    search_->setFocus();
}

void ObjectSelectionDialog::set_objects(std::vector<ObjectChoice> objects) {
    rebuilding_ = true;
    const QSignalBlocker tree_guard(tree_), kind_guard(kind_);
    objects_ = std::move(objects);
    const int previous_kind = kind_->count() ? kind_->currentData().toInt() : -1;
    kind_->clear();
    kind_->addItem(QStringLiteral("全部类型"), -1);
    for (const auto kind :
         {ObjectKind::satellite, ObjectKind::ground_station, ObjectKind::sensor, ObjectKind::other})
        if (std::ranges::any_of(objects_,
                                [kind](const auto &o) { return o.selectable && o.kind == kind; }))
            kind_->addItem(kind_name(kind), static_cast<int>(kind));
    kind_->setCurrentIndex(std::max(0, kind_->findData(previous_kind)));
    kind_->setVisible(kind_->count() > 2);
    tree_->clear();
    std::unordered_map<std::string, QTreeWidgetItem *> items;
    const auto append = [&](const ObjectChoice &object, bool missing) {
        auto *item = new QTreeWidgetItem;
        item->setText(0, object.name);
        item->setIcon(0, object_icon(object.kind));
        item->setText(1, missing ? QStringLiteral("不可用") : kind_name(object.kind));
        item->setData(0, id_role, object_text(object.id));
        item->setData(0, kind_role, static_cast<int>(object.kind));
        item->setData(0, path_role, object.path.isEmpty() ? object.name : object.path);
        item->setData(0, available_role, object.selectable && !missing);
        item->setToolTip(0,
                         item->data(0, path_role).toString() + "\nID: " + object_text(object.id));
        Qt::ItemFlags flags = Qt::ItemIsEnabled;
        if (object.selectable)
            flags |= Qt::ItemIsSelectable;
        if (options_.multiple && object.selectable) {
            flags |= Qt::ItemIsUserCheckable;
            item->setCheckState(0, contains(value_.ids, object.id) ? Qt::Checked : Qt::Unchecked);
        }
        if (missing)
            item->setForeground(0, theme::color(theme::sakura));
        if (missing && !options_.multiple)
            flags = Qt::NoItemFlags;
        item->setFlags(flags);
        items.emplace(object.id, item);
    };
    for (const auto &object : objects_)
        append(object, false);
    // Attach only after creating every item, so callers need not order parents first.
    for (const auto &object : objects_) {
        auto *item = items.at(object.id);
        const auto parent = items.find(object.parent_id);
        if (!object.parent_id.empty() && parent != items.end() && parent->second != item)
            parent->second->addChild(item);
        else
            tree_->addTopLevelItem(item);
    }
    for (const auto &id : value_.ids)
        if (!items.contains(id)) {
            append({id, object_text(id) + QStringLiteral("（已删除）")}, true);
            tree_->addTopLevelItem(items.at(id));
        }
    tree_->expandAll();
    if (!value_.ids.empty() && items.contains(value_.ids.front()))
        tree_->setCurrentItem(items.at(value_.ids.front()));
    rebuilding_ = false;
    filter();
}

void ObjectSelectionDialog::filter() {
    if (rebuilding_)
        return;
    const auto query = search_->text().trimmed();
    const int kind = kind_->currentData().toInt();
    // Children precede parents when evaluating visibility; matching sensors retain context.
    const auto visit = [&](auto &&self, QTreeWidgetItem *item) -> bool {
        bool child_match = false;
        for (int i = 0; i < item->childCount(); ++i)
            child_match |= self(self, item->child(i));
        const bool own_match =
            (kind < 0 || item->data(0, kind_role).toInt() == kind) &&
            (item->data(0, path_role).toString().contains(query, Qt::CaseInsensitive) ||
             item->data(0, id_role).toString().contains(query, Qt::CaseInsensitive));
        item->setHidden(!own_match && !child_match);
        if (child_match && (!query.isEmpty() || kind >= 0))
            item->setExpanded(true);
        return own_match || child_match;
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        visit(visit, tree_->topLevelItem(i));
    update_status();
}

void ObjectSelectionDialog::update_status() {
    if (rebuilding_ || !count_)
        return;
    std::unordered_set<std::string> available;
    for (const auto &object : objects_)
        if (object.selectable)
            available.insert(object.id);
    std::size_t missing = 0;
    for (const auto &id : value_.ids)
        if (!available.contains(id))
            ++missing;
    int visible = 0;
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if (!(*it)->isHidden() && (*it)->data(0, available_role).toBool())
            ++visible;
    empty_->setText(available.empty() ? QStringLiteral("暂无可选对象")
                                      : QStringLiteral("没有匹配的对象\n请更换名称、路径或 ID"));
    empty_->setVisible(visible == 0);
    count_->setText(value_.all ? options_.all_label
                               : QStringLiteral("已选 %1 个 · 显示 %2 / %3 个")
                                     .arg(value_.ids.size())
                                     .arg(visible)
                                     .arg(available.size()));
    if (!value_.all && missing)
        count_->setText(count_->text() +
                        QStringLiteral("\n%1 个对象不可用，请重新选择。").arg(missing));
    confirm_->setEnabled(value_.all || (!value_.ids.empty() && missing == 0));
    clear_->setEnabled(!value_.all && !value_.ids.empty());
    auto *current = tree_->currentItem();
    const auto path = current ? current->data(0, path_role).toString() : QString{};
    const auto id = current ? current->data(0, id_role).toString() : QString{};
    const int width = std::max(1, details_->width());
    details_->setText(
        current ? details_->fontMetrics().elidedText(path, Qt::ElideMiddle, width) + "\n" +
                      details_->fontMetrics().elidedText("ID: " + id, Qt::ElideMiddle, width)
                : QStringLiteral("选择对象可查看完整路径和 ID"));
    details_->setToolTip(path + "\nID: " + id);
}
void ObjectSelectionDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    update_status();
}
ObjectSelection ObjectSelectionDialog::selection() const {
    return value_.all ? ObjectSelection{{}, true} : value_;
}
void ObjectSelectionDialog::confirm() {
    update_status();
    if (confirm_->isEnabled())
        accept();
}

ObjectSelectionField::ObjectSelectionField(QWidget *parent) : QPushButton(parent) {
    setProperty("objectSelector", true);
    setAutoDefault(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(this, &QPushButton::clicked, this, &ObjectSelectionField::choose);
    refresh();
}
void ObjectSelectionField::set_options(ObjectSelectionOptions options) {
    options_ = std::move(options);
    refresh();
}
void ObjectSelectionField::set_objects(std::vector<ObjectChoice> objects) {
    objects_ = std::move(objects);
    if (chooser_)
        chooser_->set_objects(objects_);
    refresh();
}
void ObjectSelectionField::set_selection(ObjectSelection selection) {
    if (selection_ == selection)
        return;
    selection_ = std::move(selection);
    refresh();
    emit selectionChanged();
}
void ObjectSelectionField::set_id(std::string id) {
    set_selection(
        {id.empty() ? std::vector<std::string>{} : std::vector<std::string>{std::move(id)}, false});
}
std::string ObjectSelectionField::id() const {
    return selection_.ids.empty() ? std::string{} : selection_.ids.front();
}
bool ObjectSelectionField::contains(const std::string &id) const {
    return std::ranges::any_of(objects_, [&](const auto &o) { return o.selectable && o.id == id; });
}
QSize ObjectSelectionField::sizeHint() const { return {240, QPushButton::sizeHint().height()}; }
QSize ObjectSelectionField::minimumSizeHint() const {
    return {120, QPushButton::minimumSizeHint().height()};
}
void ObjectSelectionField::refresh() {
    QString label;
    auto glyph = workbench_icon(WorkbenchIcon::search);
    if (selection_.all)
        label = options_.all_label;
    else if (selection_.ids.empty())
        label = QStringLiteral("选择对象");
    else if (selection_.ids.size() > 1)
        label = QStringLiteral("已选 %1 个对象").arg(selection_.ids.size());
    else {
        const auto found = std::ranges::find(objects_, id(), &ObjectChoice::id);
        if (found != objects_.end() && found->selectable) {
            label = found->path.isEmpty() ? found->name : found->path;
            glyph = object_icon(found->kind);
        } else
            label = object_text(id()) + QStringLiteral("（已删除）");
    }
    setText(label);
    setIcon(glyph);
    setToolTip(label + (selection_.ids.size() == 1 ? "\nID: " + object_text(id()) : QString{}) +
               QStringLiteral("\n打开对象选择窗口"));
    setAccessibleDescription(label + QStringLiteral("；打开对象选择窗口"));
}
void ObjectSelectionField::paintEvent(QPaintEvent *) {
    QStyleOptionButton option;
    initStyleOption(&option);
    option.text = fontMetrics().elidedText(text(), Qt::ElideMiddle, std::max(0, width() - 70));
    QStylePainter painter(this);
    painter.drawControl(QStyle::CE_PushButton, option);
    painter.drawText(QRect(width() - 28, 0, 20, height()), Qt::AlignCenter, QStringLiteral("…"));
}
void ObjectSelectionField::choose() {
    ObjectSelectionDialog dialog(objects_, options_, selection_, this);
    chooser_ = &dialog;
    if (dialog.exec() == QDialog::Accepted)
        set_selection(dialog.selection());
    chooser_ = nullptr;
}
} // namespace orbitforge::desktop
