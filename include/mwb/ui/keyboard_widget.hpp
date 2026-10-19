#pragma once

#include <QWidget>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>

namespace mwb::ui {

/// Who owns a sounding key: each live source releases only its own notes (section 8.1).
enum class KeySource : std::uint8_t { pointer, computer_keyboard, midi_input, playback };

/// Horizontal on-screen keyboard covering MIDI 0..127 with a two-octave default viewport
/// (section 8.1). Mouse-down starts a note, mouse-up ends it, dragging across keys releases the
/// old key and starts the new one, and losing capture releases every pointer-owned note.
class KeyboardWidget : public QWidget {
    Q_OBJECT

public:
    explicit KeyboardWidget(QWidget* parent = nullptr);

    /// Velocity behaviour: fixed value, or by vertical strike position.
    void set_fixed_velocity(std::optional<int> velocity);
    /// Scale display: root pitch class and 12-bit absolute pitch-class mask.
    void set_scale(int root_pitch_class, std::uint16_t absolute_mask, bool show);
    /// Marks a key active for a source (playback, MIDI input, computer keyboard).
    void set_key_state(int pitch, KeySource source, bool down);
    /// True while `source` holds `pitch` down.
    [[nodiscard]] bool key_held(int pitch, KeySource source) const;
    /// Marks keys held only by the sustain pedal (shown differently from held keys).
    void set_sustained(std::set<int> pitches);
    /// Releases every note owned by `source` (focus loss, mode change, device change).
    void release_source(KeySource source);
    /// First visible pitch (left edge) and number of visible white keys.
    void set_view_start(int pitch);
    [[nodiscard]] int view_start() const { return view_start_; }
    /// Octave display offset (C4 = 60 by default, offset -1).
    void set_octave_offset(int offset);

    [[nodiscard]] QSize sizeHint() const override;

signals:
    void note_on(int pitch, int velocity);
    void note_off(int pitch);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool event(QEvent* event) override;

private:
    struct KeyGeometry {
        QRectF rect;
        bool black = false;
    };
    [[nodiscard]] std::optional<int> pitch_at(QPointF point) const;
    [[nodiscard]] KeyGeometry key_geometry(int pitch) const;
    [[nodiscard]] double white_key_width() const;
    [[nodiscard]] int velocity_for(QPointF point, int pitch) const;
    void press_pointer_key(int pitch, int velocity);
    void release_pointer_key();

    int view_start_ = 48; ///< C3
    double wheel_accumulated_ = 0; ///< Wheel notches not yet turned into a scroll step.
    int visible_white_keys_ = 14;
    int octave_offset_ = -1;
    std::optional<int> fixed_velocity_ = 100;
    int root_ = 0;
    std::uint16_t scale_mask_ = 0x0FFF;
    bool show_scale_ = false;
    std::optional<int> pointer_key_;
    std::optional<int> hover_key_;
    std::array<std::uint8_t, 128> owners_{}; ///< Bit set of KeySource per key.
    std::set<int> sustained_;
};

/// Default lower-row map of section 8.2: A W S E D F T G Y H U J K = semitones 0..12.
struct ComputerKeyboardMap {
    std::map<int, int> semitone_by_key; ///< Qt::Key -> semitone above the octave root.
    int octave_key_down = 0;            ///< Qt::Key_Z
    int octave_key_up = 0;              ///< Qt::Key_X
    int velocity_key_down = 0;          ///< Qt::Key_C
    int velocity_key_up = 0;            ///< Qt::Key_V

    [[nodiscard]] static ComputerKeyboardMap defaults();
};

/// Computer-keyboard playing mode (section 8.2): ignores auto-repeat, releases with the note
/// captured at key-down even if octave or velocity change while held, and releases only its own
/// notes on deactivation.
class ComputerKeyboardPlayer : public QObject {
    Q_OBJECT

public:
    explicit ComputerKeyboardPlayer(QObject* parent = nullptr);

    void set_enabled(bool enabled);
    [[nodiscard]] bool enabled() const { return enabled_; }
    void set_map(ComputerKeyboardMap map) { map_ = std::move(map); }
    [[nodiscard]] int octave_root() const { return octave_root_; }
    [[nodiscard]] int velocity() const { return velocity_; }
    /// Handles a key event; returns true when it was consumed by playing mode.
    bool handle_key(QKeyEvent* event, bool pressed);
    /// Releases all notes this mode owns (window deactivation, text focus, mode off).
    void release_all();

signals:
    void note_on(int pitch, int velocity);
    void note_off(int pitch);
    void state_changed(int octave_root, int velocity);

private:
    ComputerKeyboardMap map_ = ComputerKeyboardMap::defaults();
    bool enabled_ = false;
    int octave_root_ = 60;
    int velocity_ = 100;
    std::map<int, int> held_; ///< key -> pitch captured at key-down
};

} // namespace mwb::ui
