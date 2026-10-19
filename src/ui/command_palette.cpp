#include "mwb/ui/command_palette.hpp"

#include "mwb/core/text.hpp"

#include <QAction>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QVBoxLayout>

#include <rapidfuzz/fuzz.hpp>

#include <algorithm>

namespace mwb::ui {

CommandPalette::CommandPalette(const std::vector<QAction*>& commands, QWidget* parent) : QDialog(parent) {
    for (auto* action : commands)
        commands_.emplace_back(action);
    setWindowTitle(tr("Command Palette"));
    query_ = new QLineEdit(this);
    query_->setPlaceholderText(tr("Type a command…"));
    query_->setAccessibleName(tr("Command search"));
    list_ = new QListWidget(this);
    list_->setAccessibleName(tr("Matching commands"));
    reason_ = new QLabel(this);
    reason_->setWordWrap(true);
    reason_->hide();
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(query_);
    layout->addWidget(list_);
    layout->addWidget(reason_);
    resize(560, 420);
    query_->installEventFilter(this);
    connect(query_, &QLineEdit::textChanged, this, &CommandPalette::refilter);
    connect(list_, &QListWidget::itemActivated, this, &CommandPalette::run_selected);
    connect(list_, &QListWidget::currentRowChanged, this, [this] { reason_->hide(); });
    refilter();
}

void CommandPalette::set_query(const QString& text) {
    query_->setText(text);
    query_->setCursorPosition(static_cast<int>(text.size()));
    refilter();
}

QAction* CommandPalette::action_at(int row) const {
    const auto* item = list_->item(row);
    if (!item)
        return nullptr;
    const auto index = item->data(Qt::UserRole).toLongLong();
    if (index < 0 || static_cast<std::size_t>(index) >= commands_.size())
        return nullptr;
    return commands_[static_cast<std::size_t>(index)].data(); // null once the action is gone
}

void CommandPalette::refilter() {
    const std::string query = fold_for_search(query_->text().toStdString());
    struct Scored {
        double score;
        bool enabled;
        std::size_t index;
    };
    std::vector<Scored> scored;
    for (std::size_t index = 0; index < commands_.size(); ++index) {
        QAction* action = commands_[index].data();
        if (!action)
            continue;
        const std::string text = fold_for_search(action->text().remove('&').toStdString());
        const double score = query.empty() ? 100.0 : rapidfuzz::fuzz::partial_ratio(query, text);
        if (score >= 60.0)
            scored.push_back({score, action->isEnabled(), index});
    }
    // Runnable commands first, then by score: Enter never lands on an unavailable command.
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        return a.enabled != b.enabled ? a.enabled : a.score > b.score;
    });
    list_->clear();
    reason_->hide();
    int first_enabled = -1;
    const QColor muted = palette().color(QPalette::PlaceholderText);
    for (const auto& entry : scored) {
        QAction* action = commands_[entry.index].data();
        QString label = action->text().remove('&');
        if (action->isCheckable())
            label += action->isChecked() ? tr("  (on)") : tr("  (off)"); // toggles show their state
        if (!action->shortcut().isEmpty())
            label += "    " + action->shortcut().toString(QKeySequence::NativeText); // names its current shortcut
        if (!action->isEnabled())
            label += "    (" + (action->toolTip().isEmpty() ? tr("unavailable") : action->toolTip()) + ")";
        auto* item = new QListWidgetItem(label, list_);
        item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(static_cast<qlonglong>(entry.index)));
        // Unavailable rows stay readable (placeholder color, 5:1), not disabled gray (2.4:1): the
        // reason is there to be read.
        if (!action->isEnabled())
            item->setForeground(muted);
        else if (first_enabled < 0)
            first_enabled = list_->count() - 1;
    }
    if (list_->count() > 0)
        list_->setCurrentRow(first_enabled >= 0 ? first_enabled : 0);
}

void CommandPalette::run_selected() {
    QAction* action = action_at(list_->currentRow());
    if (!action)
        return;
    if (!action->isEnabled()) {
        // Stay open and say why, instead of closing without doing anything.
        reason_->setText(tr("%1 is unavailable: %2")
                             .arg(action->text().remove('&'), action->toolTip().isEmpty() ? tr("not possible right now")
                                                                                        : action->toolTip()));
        reason_->show();
        return;
    }
    accept();
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
