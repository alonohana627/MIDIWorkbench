#pragma once

#include <QDialog>
#include <QKeySequence>
#include <QString>
#include <QStringList>

#include <vector>

class QAction;
class QLineEdit;
class QTableWidget;

namespace mwb::ui {

/// Shortcut profile as versioned JSON: {"format":"mwb.shortcuts","version":1,"bindings":{id: "Ctrl+S"}}.
[[nodiscard]] QString shortcuts_to_json(const std::vector<QAction*>& commands);
/// Applies a profile; unknown command IDs and unreadable bindings are ignored and reported in
/// `messages` (an unreadable binding keeps the current one).
bool apply_shortcuts_json(const std::vector<QAction*>& commands, const QString& json, QStringList* messages);

/// Shortcut editor (section 7.5): lists commands with context, current and default bindings (the
/// command ID column is shown on request), filters by name or key, flags conflicts (extra
/// bindings included) and refuses OK while any remain, and exports/imports profiles. Changes apply
/// on OK.
class ShortcutEditor : public QDialog {
    Q_OBJECT

public:
    ShortcutEditor(std::vector<QAction*> commands, QWidget* parent = nullptr);

private:
    void populate();
    void mark_conflicts();
    void apply_filter();
    void apply();
    void export_profile();
    void import_profile();
    /// Bindings a row would have after OK: the edited one plus kept extra bindings.
    [[nodiscard]] std::vector<QKeySequence> bindings_of_row(int row) const;
    /// "Ctrl+S: Save, Other" for each binding used by more than one command.
    [[nodiscard]] QStringList conflict_descriptions() const;

    std::vector<QAction*> commands_;
    QLineEdit* filter_ = nullptr;
    QTableWidget* table_ = nullptr;
};

} // namespace mwb::ui
