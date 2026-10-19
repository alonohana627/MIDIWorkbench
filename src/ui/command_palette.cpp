#include "mwb/ui/command_palette.hpp"

#include "mwb/core/text.hpp"

#include <QAction>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

#include <rapidfuzz/fuzz.hpp>

#include <algorithm>

namespace mwb::ui {

CommandPalette::CommandPalette(const std::vector<QAction*>& commands, QWidget* parent)
    : QDialog(parent), commands_(commands) {
    setWindowTitle(tr("Command Palette"));
    query_ = new QLineEdit(this);
    query_->setPlaceholderText(tr("Type a command…"));
    query_->setAccessibleName(tr("Command search"));
    list_ = new QListWidget(this);
    list_->setAccessibleName(tr("Matching commands"));
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(query_);
    layout->addWidget(list_);
    resize(560, 420);
    query_->installEventFilter(this);
    connect(query_, &QLineEdit::textChanged, this, &CommandPalette::refilter);
    connect(list_, &QListWidget::itemActivated, this, &CommandPalette::run_selected);
    refilter();
}

void CommandPalette::refilter() {
    const std::string query = fold_for_search(query_->text().toStdString());
    std::vector<std::pair<double, QAction*>> scored;
    for (auto* action : commands_) {
        const std::string text = fold_for_search(action->text().remove('&').toStdString());
        const double score = query.empty() ? 100.0 : rapidfuzz::fuzz::partial_ratio(query, text);
        if (score >= 60.0)
            scored.emplace_back(score, action);
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    list_->clear();
    for (const auto& [score, action] : scored) {
        QString label = action->text().remove('&');
        if (!action->shortcut().isEmpty())
            label += "    " + action->shortcut().toString(QKeySequence::NativeText); // names its current shortcut
        if (!action->isEnabled())
            label += "    (" + (action->toolTip().isEmpty() ? tr("unavailable") : action->toolTip()) + ")";
        auto* item = new QListWidgetItem(label, list_);
        item->setData(Qt::UserRole, QVariant::fromValue(static_cast<void*>(action)));
        if (!action->isEnabled())
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
    }
    if (list_->count() > 0)
        list_->setCurrentRow(0);
}

void CommandPalette::run_selected() {
    auto* item = list_->currentItem();
    if (!item)
        return;
    auto* action = static_cast<QAction*>(item->data(Qt::UserRole).value<void*>());
    accept();
    if (action && action->isEnabled())
        action->trigger();
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event) {
    if (watched == query_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
            const int row = std::clamp(list_->currentRow() + (key->key() == Qt::Key_Down ? 1 : -1), 0, list_->count() - 1);
            list_->setCurrentRow(row);
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            run_selected();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace mwb::ui
