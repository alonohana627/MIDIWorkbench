// MIDI Workbench desktop application entry point.
//
// Headless switches for agents and CI (specification section 32.2):
//   --workspace <dir>       use <dir> for preferences, logs and recovery data
//   --exit-when-ready       start, build the window, and exit 0 once the event loop is running
//   --screenshot <file.png> save a screenshot of the main window (implies a ready window)
//   --theme <name>          system, dark, light or high_contrast for this run (not saved)
//   QT_QPA_PLATFORM=offscreen, MWB_AUDIO_BACKEND=null, MWB_MIDI_BACKEND=null for no devices.
#include "mwb/app/environment.hpp"
#include "mwb/document/recovery.hpp"
#include "mwb/ui/main_window.hpp"
#include "mwb/ui/theme.hpp"

#include <CLI/CLI.hpp>

#include <QApplication>
#include <QIcon>
#include <QMessageBox>
#include <QSettings>
#include <QStringList>

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace {

/// The executable uses the GUI subsystem: started from Explorer or a shortcut it has no console,
/// so errors and --help are also shown in a message box. Headless runs (QT_QPA_PLATFORM set, as in
/// tests and CI) keep stderr only.
void report(const std::string& text, bool error) {
    std::fprintf(error ? stderr : stdout, "%s\n", text.c_str());
    if (qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        return;
    if (error)
        QMessageBox::critical(nullptr, QStringLiteral("MIDI Workbench"), QString::fromStdString(text));
    else
        QMessageBox::information(nullptr, QStringLiteral("MIDI Workbench"), QString::fromStdString(text));
}

} // namespace

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
        // --help and --version are "errors" with exit code 0; both are shown, not only printed.
        std::ostringstream out;
        std::ostringstream err;
        const int code = command_line.exit(error, out, err);
        report(code == 0 ? out.str() : err.str() + out.str(), code != 0);
        return code;
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
        report("error: the data folder could not be created: " + created.error().describe(), true);
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
    mwb::ui::apply_base_font(application, QSettings().value("ui/font_step", 0).toInt());
    mwb::ui::apply_theme(mwb::ui::theme_from_name(
        theme.empty() ? QSettings().value("ui/theme", "system").toString() : QString::fromStdString(theme)));

    int code = 0;
    {
        mwb::ui::MainWindow window(options, directories);
        if (!options.exit_when_ready || !options.screenshot.empty())
            window.show();
        code = application.exec();
    } // the window drains its recovery thread here
    // Queued checkpoint serialization is no longer needed: drop it and join the threads, so exit
    // never waits for it or runs it during static destruction (code-document-18).
    mwb::document::shutdown_recovery_executors();
    return code;
}
