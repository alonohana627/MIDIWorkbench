#include "mwb/music/transform.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <tuple>

namespace mwb::music {

Result<TransformPreview> remove_duplicates(const DocumentData& document, const TargetSelection& selection,
                                           const RemoveDuplicatesParameters& parameters) {
    auto prepared = prepare_transform(document, selection, "remove_duplicates", false);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    std::vector<std::size_t> ordered = selected;
    std::ranges::sort(ordered);
    using Key = std::tuple<std::size_t, int, int, Tick, Tick, int, int>;
    std::map<Key, std::size_t> keeper;
    for (const auto note : ordered) {
        const auto values = editor.values(note);
        const std::size_t track = parameters.across_tracks ? 0 : editor.notes()[note].on_track;
        Key key{track, values.channel, values.pitch, values.start, 0, 0, 0};
        switch (parameters.policy) {
        case DuplicatePolicy::exact:
            key = Key{track, values.channel, values.pitch, values.start, values.end, values.velocity,
                      values.release_velocity};
            break;
        case DuplicatePolicy::ignore_velocity:
            key = Key{track, values.channel, values.pitch, values.start, values.end, 0, 0};
            break;
        case DuplicatePolicy::same_onset: break;
        }
        const auto found = keeper.find(key);
        if (found == keeper.end()) {
            keeper.emplace(key, note);
            continue;
        }
        if (parameters.policy == DuplicatePolicy::same_onset) {
            const auto kept = editor.values(found->second);
            if (values.end > kept.end) {
                editor.remove(found->second);
                found->second = note;
                ++report.removed;
                continue;
            }
        }
        editor.remove(note);
        ++report.removed;
    }
    const std::string_view policy = parameters.policy == DuplicatePolicy::exact            ? "exact duplicates"
                                    : parameters.policy == DuplicatePolicy::ignore_velocity ? "same span, any velocity"
                                                                                            : "same onset, longest kept";
    report.messages.push_back(std::format("{} duplicate notes removed (policy: {}{})", report.removed, policy,
                                          parameters.across_tracks ? ", across tracks" : ", within each track"));
    return finish_preview(editor, std::move(report), "Remove duplicates");
}

Result<TransformPreview> resolve_overlaps(const DocumentData& document, const TargetSelection& selection,
                                          const ResolveOverlapsParameters& parameters) {
    auto prepared = prepare_transform(document, selection, "resolve_overlaps", false);
    if (!prepared)
        return std::unexpected(prepared.error());
    auto& [editor, selected, report] = *prepared;
    std::map<std::tuple<std::size_t, int, int>, std::vector<std::size_t>> lines;
    for (const auto note : selected) {
        if (editor.is_unterminated(note)) {
            ++report.skipped;
            continue;
        }
        const auto values = editor.values(note);
        lines[{editor.notes()[note].on_track, values.channel, values.pitch}].push_back(note);
    }
    std::size_t overlaps = 0;
    for (auto& [key, members] : lines) {
        std::ranges::stable_sort(members, [&](std::size_t a, std::size_t b) {
            return std::make_pair(editor.values(a).start, a) < std::make_pair(editor.values(b).start, b);
        });
        std::size_t current = members.front();
        for (std::size_t index = 1; index < members.size(); ++index) {
            const auto next = members[index];
            NoteValues earlier = editor.values(current);
            const NoteValues later = editor.values(next);
            if (later.start >= earlier.end) {
                current = next;
                continue;
            }
            ++overlaps;
            if (later.start == earlier.start) {
                ++report.ambiguous;
                current = later.end > earlier.end ? next : current;
                continue;
            }
            switch (parameters.action) {
            case OverlapAction::trim_earlier:
                earlier.end = later.start;
                editor.update(current, earlier);
                ++report.changed;
                current = next;
                break;
            case OverlapAction::merge:
                earlier.end = std::max(earlier.end, later.end);
                editor.update(current, earlier);
                editor.remove(next);
                ++report.changed;
                ++report.removed;
                break;
            case OverlapAction::leave: current = later.end > earlier.end ? next : current; break;
            }
        }
    }
    report.messages.push_back(std::format("{} same-pitch overlaps found on the same track and channel", overlaps));
    if (report.ambiguous > 0)
        report.messages.push_back(
            std::format("{} overlaps start at the same tick and were left for review", report.ambiguous));
    return finish_preview(editor, std::move(report), "Resolve overlaps");
}

} // namespace mwb::music
