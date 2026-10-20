#include "mwb/ui/instrument_menu.hpp"

#include "mwb/audio/builtin_synth.hpp"
#include "mwb/ui/document_controller.hpp"

#include <QAction>
#include <QActionGroup>
#include <QCoreApplication>
#include <QMenu>

namespace mwb::ui {

namespace {

QString tr(const char* text) { return QCoreApplication::translate("InstrumentMenu", text); }

} // namespace

void populate_instrument_menu(QMenu& menu, const music::TrackInstrument& current,
                              const std::function<void(std::optional<std::uint8_t>)>& chosen) {
    auto* group = new QActionGroup(&menu); // one checked entry across all submenus
    QAction* current_family = nullptr;
    for (int family = 0; family < music::gm_family_count; ++family) {
        auto* submenu = menu.addMenu(QString::fromUtf8(music::gm_family_name(family).data(),
                                                       static_cast<qsizetype>(music::gm_family_name(family).size())));
        submenu->setObjectName(QStringLiteral("instrumentFamily%1").arg(family));
        for (int offset = 0; offset < music::gm_family_size; ++offset) {
            const auto program = static_cast<std::uint8_t>(family * music::gm_family_size + offset);
            const auto name = music::gm_program_name(program);
            auto* action = submenu->addAction(QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())));
            action->setCheckable(true);
            action->setActionGroup(group);
            action->setData(static_cast<int>(program));
            action->setToolTip(tr("General MIDI program %1").arg(program + 1));
            if (!current.drums && current.program == program) {
                action->setChecked(true);
                current_family = submenu->menuAction();
            }
            QObject::connect(action, &QAction::triggered, &menu, [chosen, program] { chosen(program); });
        }
    }
    menu.addSeparator();
    auto* drums = menu.addAction(tr("Drums (channel 10)"));
    drums->setObjectName(QStringLiteral("instrumentDrums"));
    drums->setCheckable(true);
    drums->setActionGroup(group);
    drums->setChecked(current.drums);
    drums->setData(-1);
    QObject::connect(drums, &QAction::triggered, &menu, [chosen] { chosen(std::nullopt); });
    menu.setActiveAction(current.drums ? drums : current_family);
}

QString choose_track_instrument(DocumentController& controller, TrackId track_id, std::optional<std::uint8_t> program) {
    const auto location = find_track(controller.document(), track_id);
    if (!location)
        return tr("That track no longer exists");
    const QString track_name =
        QString::fromStdString(controller.document().sequences[location->sequence].tracks[location->track].name);
    const std::string timbre =
        program ? std::string(audio::timbre_name(audio::timbre_for_program(*program))) : std::string();
    auto edit = music::set_track_instrument(controller.document(), track_id, program, timbre);
    if (!edit)
        return QString::fromStdString(edit.error().message);
    const ChangeSet* changes = &edit->changes;
    const QString label = program ? QString::fromUtf8(music::gm_program_name(*program).data(),
                                                      static_cast<qsizetype>(music::gm_program_name(*program).size()))
                                  : tr("Drums");
    if (changes->empty())
        return tr("%1 already plays %2").arg(track_name, label);
    if (QString error; !controller.apply(*changes, &error))
        return tr("Instrument not changed: %1").arg(error);
    if (!program)
        return tr("%1 is now a drum track (channel 10)").arg(track_name);
    QString message = tr("%1 now plays %2 (program %3; built-in sound \"%4\"; saved in the file as a Program Change)")
                          .arg(track_name, label)
                          .arg(*program + 1)
                          .arg(QString::fromStdString(timbre));
    if (edit->moved_to)
        message += tr("; its notes moved from channel 10 (drums) to channel %1").arg(*edit->moved_to + 1);
    return message;
}

} // namespace mwb::ui
