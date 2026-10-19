#include "mwb/ui/shortcut_editor.hpp"

#include <QAction>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <map>

namespace mwb::ui {

namespace {

constexpr int column_id = 0;
constexpr int column_text = 1;
constexpr int column_context = 2;
constexpr int column_shortcut = 3;
constexpr int column_default = 4;

QString context_name(Qt::ShortcutContext context) {
    switch (context) {
    case Qt::ApplicationShortcut: return QObject::tr("Global");
    case Qt::WindowShortcut: return QObject::tr("Main window (not while typing in a text field)");
    case Qt::WidgetShortcut:
    case Qt::WidgetWithChildrenShortcut: return QObject::tr("Focused panel");
    }
    return {};
}

} // namespace

QString shortcuts_to_json(const std::vector<QAction*>& commands) {
    QJsonObject bindings;
    for (const auto* action : commands)
        bindings.insert(action->data().toString(), action->shortcut().toString(QKeySequence::PortableText));
    QJsonObject root{{"format", "mwb.shortcuts"}, {"version", 1}, {"bindings", bindings}};
    return QString::fromUtf8(QJsonDocument(root).toJson());
}

bool apply_shortcuts_json(const std::vector<QAction*>& commands, const QString& json, QStringList* messages) {
    if (json.size() > 256 * 1024)
        return false; // bounded like any imported preset (section 12.1)
    const auto document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isObject() || document.object().value("format").toString() != "mwb.shortcuts" ||
        document.object().value("version").toInt() != 1)
        return false;
    const auto bindings = document.object().value("bindings").toObject();
    std::map<QString, QAction*> by_id;
    for (auto* action : commands)
        by_id[action->data().toString()] = action;
    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
        const auto found = by_id.find(it.key());
        if (found == by_id.end()) {
            if (messages)
                *messages << QObject::tr("Unknown command %1 ignored").arg(it.key());
            continue;
        }
        found->second->setShortcut(QKeySequence::fromString(it.value().toString(), QKeySequence::PortableText));
    }
    return true;
}

ShortcutEditor::ShortcutEditor(std::vector<QAction*> commands, QWidget* parent)
    : QDialog(parent), commands_(std::move(commands)) {
    setWindowTitle(tr("Keyboard Shortcuts"));
    table_ = new QTableWidget(this);
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels({tr("Command ID"), tr("Command"), tr("Context"), tr("Shortcut"), tr("Default")});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->verticalHeader()->hide();
    table_->setAccessibleName(tr("Shortcuts"));
    auto* note = new QLabel(tr("Conflicting shortcuts are marked. Text fields always receive typing first."), this);
    auto* export_button = new QPushButton(tr("Export…"), this);
    auto* import_button = new QPushButton(tr("Import…"), this);
    auto* reset_button = new QPushButton(tr("Reset to Defaults"), this);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* tools = new QHBoxLayout;
    tools->addWidget(export_button);
    tools->addWidget(import_button);
    tools->addWidget(reset_button);
    tools->addStretch(1);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note);
    layout->addWidget(table_, 1);
    layout->addLayout(tools);
    layout->addWidget(buttons);
    resize(900, 600);
    populate();
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        apply();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(export_button, &QPushButton::clicked, this, &ShortcutEditor::export_profile);
    connect(import_button, &QPushButton::clicked, this, &ShortcutEditor::import_profile);
    connect(reset_button, &QPushButton::clicked, this, [this] {
        for (int row = 0; row < table_->rowCount(); ++row)
            if (auto* editor = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(row, column_shortcut)))
                editor->setKeySequence(QKeySequence(table_->item(row, column_default)->text(), QKeySequence::NativeText));
        mark_conflicts();
    });
}

void ShortcutEditor::populate() {
    table_->setRowCount(static_cast<int>(commands_.size()));
    for (std::size_t index = 0; index < commands_.size(); ++index) {
        const int row = static_cast<int>(index);
        auto* action = commands_[index];
        table_->setItem(row, column_id, new QTableWidgetItem(action->data().toString()));
        table_->setItem(row, column_text, new QTableWidgetItem(action->text().remove('&')));
        table_->setItem(row, column_context, new QTableWidgetItem(context_name(action->shortcutContext())));
        const auto default_shortcut = action->property("mwb_default_shortcut").value<QKeySequence>();
        table_->setItem(row, column_default, new QTableWidgetItem(default_shortcut.toString(QKeySequence::NativeText)));
        auto* editor = new QKeySequenceEdit(action->shortcut(), table_);
        editor->setMaximumSequenceLength(1);
        connect(editor, &QKeySequenceEdit::keySequenceChanged, this, &ShortcutEditor::mark_conflicts);
        table_->setCellWidget(row, column_shortcut, editor);
    }
    mark_conflicts();
}

void ShortcutEditor::mark_conflicts() {
    std::map<QString, int> counts;
    for (int row = 0; row < table_->rowCount(); ++row)
        if (auto* editor = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(row, column_shortcut)))
            if (!editor->keySequence().isEmpty())
                ++counts[editor->keySequence().toString()];
    for (int row = 0; row < table_->rowCount(); ++row) {
        auto* editor = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(row, column_shortcut));
        const bool conflict = editor && !editor->keySequence().isEmpty() && counts[editor->keySequence().toString()] > 1;
        // Text as well as color marks a conflict (section 29.3).
        table_->item(row, column_text)->setText(commands_[static_cast<std::size_t>(row)]->text().remove('&') +
                                                (conflict ? tr("  [conflict]") : QString()));
        table_->item(row, column_text)->setForeground(conflict ? QBrush(QColor(200, 40, 40)) : QBrush());
    }
}

void ShortcutEditor::apply() {
    for (int row = 0; row < table_->rowCount(); ++row)
        if (auto* editor = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(row, column_shortcut))) {
            auto* action = commands_[static_cast<std::size_t>(row)];
            if (editor->keySequence() != action->shortcut()) // unchanged rows keep extra bindings
                action->setShortcut(editor->keySequence());
        }
}

void ShortcutEditor::export_profile() {
    const QString path = QFileDialog::getSaveFileName(this, tr("Export shortcuts"), "shortcuts.json", tr("JSON (*.json)"));
    if (path.isEmpty())
        return;
    // Export the edited table without committing it: OK/Cancel still decide.
    std::vector<QList<QKeySequence>> previous;
    for (auto* action : commands_)
        previous.push_back(action->shortcuts());
    apply();
    const QString json = shortcuts_to_json(commands_);
    for (std::size_t index = 0; index < commands_.size(); ++index)
        commands_[index]->setShortcuts(previous[index]);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(json.toUtf8()) < 0)
        QMessageBox::warning(this, tr("Export shortcuts"), tr("Could not write %1").arg(path));
}

void ShortcutEditor::import_profile() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Import shortcuts"), {}, tr("JSON (*.json)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024) {
        QMessageBox::warning(this, tr("Import shortcuts"), tr("Could not read %1").arg(path));
        return;
    }
    // The profile fills the table only; OK applies it, Cancel leaves the current bindings.
    std::vector<QList<QKeySequence>> previous;
    for (auto* action : commands_)
        previous.push_back(action->shortcuts());
    QStringList messages;
    const bool read = apply_shortcuts_json(commands_, QString::fromUtf8(file.readAll()), &messages);
    if (read)
        populate();
    for (std::size_t index = 0; index < commands_.size(); ++index)
        commands_[index]->setShortcuts(previous[index]);
    if (!read) {
        QMessageBox::warning(this, tr("Import shortcuts"), tr("This is not a MIDI Workbench shortcut profile"));
        return;
    }
    if (!messages.isEmpty())
        QMessageBox::information(this, tr("Import shortcuts"), messages.join('\n'));
}

} // namespace mwb::ui
