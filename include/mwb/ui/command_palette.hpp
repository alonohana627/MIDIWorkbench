#pragma once

#include <QDialog>
#include <QPointer>

#include <vector>

class QAction;
class QLabel;
class QLineEdit;
class QListWidget;

namespace mwb::ui {

/// Command palette (Ctrl-Shift-P): fuzzy search over every command with its current shortcut,
/// scored with rapidfuzz (section 4.4.1). Runnable commands come first; disabled commands are
/// listed after them with their reason, and Enter on one explains instead of closing. Toggles
/// show whether they are on.
class CommandPalette : public QDialog {
    Q_OBJECT

public:
    CommandPalette(const std::vector<QAction*>& commands, QWidget* parent = nullptr);
    /// Starts with this query (text typed into the toolbar search before the palette opened).
    void set_query(const QString& text);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refilter();
    void run_selected();
    /// The command shown in `row` (null when it no longer exists).
    [[nodiscard]] QAction* action_at(int row) const;

    std::vector<QPointer<QAction>> commands_;
    QLineEdit* query_ = nullptr;
    QListWidget* list_ = nullptr;
    QLabel* reason_ = nullptr;
};

} // namespace mwb::ui
