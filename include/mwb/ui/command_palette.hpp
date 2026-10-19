#pragma once

#include <QDialog>

#include <vector>

class QAction;
class QLineEdit;
class QListWidget;

namespace mwb::ui {

/// Command palette (Ctrl-Shift-P): fuzzy search over every command with its current shortcut,
/// scored with rapidfuzz (section 4.4.1). Disabled commands are listed with their reason.
class CommandPalette : public QDialog {
    Q_OBJECT

public:
    CommandPalette(const std::vector<QAction*>& commands, QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refilter();
    void run_selected();

    std::vector<QAction*> commands_;
    QLineEdit* query_ = nullptr;
    QListWidget* list_ = nullptr;
};

} // namespace mwb::ui
