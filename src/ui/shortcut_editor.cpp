#include "mwb/ui/shortcut_editor.hpp"

#include "mwb/ui/theme.hpp"

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
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <map>

namespace mwb::ui {

namespace {

// The readable name comes first; the internal command ID is last and hidden by default.
constexpr int column_text = 0;
constexpr int column_context = 1;
constexpr int column_shortcut = 2;
constexpr int column_default = 3;
constexpr int column_id = 4;
constexpr int column_count = 5;

/// The default binding recorded when the command was created; commands without one keep their
/// current binding on "Reset to Defaults" rather than losing it.
QKeySequence default_shortcut_of(const QAction* action) {
    const QVariant value = action->property("mwb_default_shortcut");
    return value.isValid() ? value.value<QKeySequence>() : action->shortcut();
}

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
        const QString text = it.value().toString();
        const QKeySequence sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
        if (sequence.isEmpty() && !text.trimmed().isEmpty()) {
            // An unreadable binding keeps the current one instead of silently clearing it.
            if (messages)
                *messages << QObject::tr("Shortcut \"%1\" for %2 could not be read; kept the current one").arg(text, it.key());
            continue;
        }
        found->second->setShortcut(sequence);
    }
    return true;
}

ShortcutEditor::ShortcutEditor(std::vector<QAction*> commands, QWidget* parent)
    : QDialog(parent), commands_(std::move(commands)) {
    setWindowTitle(tr("Keyboard Shortcuts"));
    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(tr("Filter by command or shortcut"));
    filter_->setAccessibleName(tr("Filter shortcuts"));
    filter_->setClearButtonEnabled(true);
    table_ = new QTableWidget(this);
    table_->setColumnCount(column_count);
    table_->setHorizontalHeaderLabels({tr("Command"), tr("Context"), tr("Shortcut"), tr("Default"), tr("Command ID")});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(column_text, QHeaderView::Stretch);
    table_->setColumnHidden(column_id, true);
    table_->verticalHeader()->hide();
    table_->setAccessibleName(tr("Shortcuts"));
    auto* note = new QLabel(tr("Conflicting shortcuts are marked and must be resolved before OK. Use the clear button "
                               "in a shortcut field to remove a binding. Text fields always receive typing first."),
                            this);
    note->setWordWrap(true);
    auto* show_ids = new QPushButton(tr("Show Command IDs"), this);
    show_ids->setCheckable(true);
    auto* export_button = new QPushButton(tr("Export…"), this);
    auto* import_button = new QPushButton(tr("Import…"), this);
    auto* reset_button = new QPushButton(tr("Reset to Defaults"), this);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* tools = new QHBoxLayout;
    tools->addWidget(export_button);
    tools->addWidget(import_button);
    tools->addWidget(reset_button);
    tools->addStretch(1);
    tools->addWidget(show_ids);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note);
    layout->addWidget(filter_);
    layout->addWidget(table_, 1);
    layout->addLayout(tools);
    layout->addWidget(buttons);
    resize(900, 600);
    populate();
    connect(filter_, &QLineEdit::textChanged, this, &ShortcutEditor::apply_filter);
    connect(show_ids, &QPushButton::toggled, this, [this](bool on) { table_->setColumnHidden(column_id, !on); });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        // Ambiguous bindings make Qt run neither command, so OK refuses them.
        const QStringList conflicts = conflict_descriptions();
        if (!conflicts.isEmpty()) {
            QMessageBox::warning(this, tr("Conflicting shortcuts"),
                                 tr("These shortcuts are used by more than one command. Change or clear one of each:\n\n%1")
                                     .arg(conflicts.join('\n')));
            return;
        }
        apply();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(export_button, &QPushButton::clicked, this, &ShortcutEditor::export_profile);
    connect(import_button, &QPushButton::clicked, this, &ShortcutEditor::import_profile);
    connect(reset_button, &QPushButton::clicked, this, [this] {
        for (int row = 0; row < table_->rowCount(); ++row)
            if (auto* editor = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(row, column_shortcut)))
                editor->setKeySequence(table_->item(row, column_default)->data(Qt::UserRole).value<QKeySequence>());
        mark_conflicts();
    });
}

void ShortcutEditor::populate() {
    table_->setRowCount(static_cast<int>(commands_.size()));
    for (std::size_t index = 0; index < commands_.size(); ++index) {
        const int row = static_cast<int>(index);
        auto* action = commands_[index];
        const QString text = action->text().remove('&');
        table_->setItem(row, column_id, new QTableWidgetItem(action->data().toString()));
        table_->setItem(row, column_text, new QTableWidgetItem(text));
        table_->setItem(row, column_context, new QTableWidgetItem(context_name(action->shortcutContext())));
        const auto default_shortcut = default_shortcut_of(action);
        auto* default_item = new QTableWidgetItem(default_shortcut.toString(QKeySequence::NativeText));
        default_item->setData(Qt::UserRole, QVariant::fromValue(default_shortcut));
        table_->setItem(row, column_default, default_item);
        auto* editor = new QKeySequenceEdit(action->shortcut(), table_);
        editor->setMaximumSequenceLength(1);
        editor->setClearButtonEnabled(true); // Backspace is recorded as a key, so clearing needs a button
        editor->setAccessibleName(tr("Shortcut for %1").arg(text));
        connect(editor, &QKeySequenceEdit::keySequenceChanged, this, &ShortcutEditor::mark_conflicts);
        table_->setCellWidget(row, column_shortcut, editor);
    }
    mark_conflicts();
    apply_filter();
}

std::vector<QKeySequence> ShortcutEditor::bindings_of_row(int row) const {
    std::vector<QKeySequence> result;
    auto* editor = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(row, column_shortcut));
    if (!editor)
        return result;
    if (!editor->keySequence().isEmpty())
        result.push_back(editor->keySequence());
    // An unchanged row keeps its extra bindings on OK (apply), so they take part in conflicts.
    const auto* action = commands_[static_cast<std::size_t>(row)];
    if (editor->keySequence() == action->shortcut()) {
        const auto all = action->shortcuts();
        for (qsizetype extra = 1; extra < all.size(); ++extra)
            if (!all[extra].isEmpty())
                result.push_back(all[extra]);
    }
    return result;
}

QStringList ShortcutEditor::conflict_descriptions() const {
    std::map<QString, QStringList> users;
    for (int row = 0; row < table_->rowCount(); ++row)
        for (const auto& sequence : bindings_of_row(row))
            users[sequence.toString(QKeySequence::NativeText)] << commands_[static_cast<std::size_t>(row)]->text().remove('&');
    QStringList result;
    for (const auto& [sequence, names] : users)
        if (names.size() > 1)
            result << tr("%1: %2").arg(sequence, names.join(", "));
    return result;
}

void ShortcutEditor::mark_conflicts() {
    std::map<QString, int> counts;
    for (int row = 0; row < table_->rowCount(); ++row)
        for (const auto& sequence : bindings_of_row(row))
            ++counts[sequence.toString()];
    for (int row = 0; row < table_->rowCount(); ++row) {
        bool conflict = false;
        for (const auto& sequence : bindings_of_row(row))
            conflict = conflict || counts[sequence.toString()] > 1;
        // Text as well as color marks a conflict (section 29.3); the danger color meets 4.5:1.
        table_->item(row, column_text)->setText(commands_[static_cast<std::size_t>(row)]->text().remove('&') +
                                                (conflict ? tr("  [conflict]") : QString()));
        table_->item(row, column_text)->setForeground(conflict ? QBrush(danger_color()) : QBrush());
    }
}

void ShortcutEditor::apply_filter() {
    const QString needle = filter_ ? filter_->text().trimmed() : QString();
    for (int row = 0; row < table_->rowCount(); ++row) {
        bool match = needle.isEmpty();
        if (!match) {
            const auto* action = commands_[static_cast<std::size_t>(row)];
            match = action->text().remove('&').contains(needle, Qt::CaseInsensitive) ||
                    action->data().toString().contains(needle, Qt::CaseInsensitive);
            for (const auto& sequence : bindings_of_row(row))
                match = match || sequence.toString(QKeySequence::NativeText).contains(needle, Qt::CaseInsensitive);
        }
        table_->setRowHidden(row, !match);
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
