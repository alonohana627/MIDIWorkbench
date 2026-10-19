#pragma once

#include <QDialog>
#include <QString>

#include <vector>

class QAction;
class QTableWidget;

namespace mwb::ui {

/// Shortcut profile as versioned JSON: {"format":"mwb.shortcuts","version":1,"bindings":{id: "Ctrl+S"}}.
[[nodiscard]] QString shortcuts_to_json(const std::vector<QAction*>& commands);
/// Applies a profile; unknown command IDs are ignored and reported in `messages`.
bool apply_shortcuts_json(const std::vector<QAction*>& commands, const QString& json, QStringList* messages);

/// Shortcut editor (section 7.5): lists command IDs, text, context, current and default
/// bindings, flags conflicts, and exports/imports profiles. Changes apply on OK.
class ShortcutEditor : public QDialog {
    Q_OBJECT

public:
    ShortcutEditor(std::vector<QAction*> commands, QWidget* parent = nullptr);

private:
    void populate();
    void mark_conflicts();
    void apply();
    void export_profile();
    void import_profile();

    std::vector<QAction*> commands_;
    QTableWidget* table_ = nullptr;
};

} // namespace mwb::ui
