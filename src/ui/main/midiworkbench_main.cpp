// MIDI Workbench desktop application entry point.
//
// Headless switches for agents and CI (specification section 32.2):
//   --workspace <dir>       use <dir> for preferences, logs and recovery data
//   --exit-when-ready       start, build the window, and exit 0 once the event loop is running
//   --screenshot <file.png> save a screenshot of the main window (implies a ready window)
//   --theme <name>          system, dark, light or high_contrast for this run (not saved)
//   QT_QPA_PLATFORM=offscreen, MWB_AUDIO_BACKEND=null, MWB_MIDI_BACKEND=null for no devices.
#include "mwb/app/environment.hpp"
#include "mwb/ui/main_window.hpp"
#include "mwb/ui/theme.hpp"

#include <CLI/CLI.hpp>

#include <QApplication>
#include <QIcon>
#include <QSettings>
#include <QStringList>

#include <cstdio>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName("MIDIWorkbench");
    QApplication::setApplicationName("MIDI Workbench");
    QApplication::setApplicationVersion("1.0.0");
    QApplication::setWindowIcon(QIcon(":/midiworkbench.png"));

    // Every option binds to storage that outlives the parse (section 31).
    std::string workspace;
    std::string screenshot;
    std::string theme;
    bool exit_when_ready = false;
    bool no_recovery_prompt = false;
    std::vector<std::string> files;
    CLI::App command_line{"MIDI Workbench", "MIDIWorkbench"};
    command_line.add_option("--workspace", workspace, "Data directory for preferences, logs and recovery");
    command_line.add_option("--screenshot", screenshot, "Write a PNG of the main window when ready");
    command_line.add_option("--theme", theme, "Theme for this run: system, dark, light or high_contrast");
    command_line.add_flag("--exit-when-ready", exit_when_ready, "Exit once the window is ready");
    command_line.add_flag("--no-recovery-prompt", no_recovery_prompt, "Do not offer crash recovery at startup");
    command_line.add_option("files", files, "MIDI or MIDI Workbench documents to open");
    std::vector<std::string> arguments;
    for (const auto& argument : QApplication::arguments().mid(1))
        arguments.push_back(argument.toStdString());
    std::reverse(arguments.begin(), arguments.end());
    try {
        command_line.parse(arguments);
    } catch (const CLI::ParseError& error) {
        return command_line.exit(error);
    }

    mwb::ui::StartupOptions options;
    options.workspace = std::filesystem::path(std::u8string(workspace.begin(), workspace.end()));
    options.screenshot = std::filesystem::path(std::u8string(screenshot.begin(), screenshot.end()));
    options.exit_when_ready = exit_when_ready;
    options.skip_recovery_prompt = no_recovery_prompt;
    for (const auto& file : files)
        options.files.emplace_back(std::u8string(file.begin(), file.end()));

    const auto directories = mwb::app::resolve_data_directories(options.workspace);
    if (auto created = mwb::app::ensure_data_directories(directories); !created) {
        std::fprintf(stderr, "error: %s\n", created.error().describe().c_str());
        return 3;
    }
    if (auto logging = mwb::app::start_logging(directories.logs); !logging)
        std::fprintf(stderr, "warning: %s\n", logging.error().describe().c_str());
    mwb::app::log_event("app", "start", "MIDI Workbench starting");

    // --workspace isolates preferences too: QSettings go to an INI file in that directory
    // instead of the user's registry (tests and screenshots never touch real settings).
    if (!workspace.empty()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QString::fromStdString(workspace));
    }
    // Font and theme before any widget exists, so every widget inherits them.
    mwb::ui::apply_base_font(application);
    mwb::ui::apply_theme(mwb::ui::theme_from_name(
        theme.empty() ? QSettings().value("ui/theme", "system").toString() : QString::fromStdString(theme)));

    mwb::ui::MainWindow window(options, directories);
    if (!options.exit_when_ready || !options.screenshot.empty())
        window.show();
    return application.exec();
}
