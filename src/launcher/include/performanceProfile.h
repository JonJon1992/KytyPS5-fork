#ifndef PERFORMANCE_PROFILE_H
#define PERFORMANCE_PROFILE_H

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

class Configuration;

// A per-game set of emulator switches, read from profiles/*.json next to the launcher:
//
//   {
//     "name": "Crash Bandicoot 4",
//     "title_ids": ["PPSA02433"],
//     "folder_names": [],
//     "game_patch": "",
//     "notes": "...",
//     "environment": { "KYTY_UFFD_WP": "1" }
//   }
//
// "environment" holds KYTY_*/TRACY_* string values, applied over the bundled u59-preset.json
// (and the shell's environment) when the game launches; the launcher's own per-game switches
// (DCC GPU clear, program cache, pipeline library, GPU fault report) still win. "folder_names"
// match a substring of the game folder's name, case-insensitively, for copies without a title
// id. "game_patch" is a patch plan relative to the launcher's directory, passed with
// --game-patch when the title has no _Patches/<TITLE_ID>.json of its own.
struct PerformanceProfile {
	QString                       id; // file name without .json
	QString                       name;
	QString                       notes;
	QStringList                   title_ids;
	QStringList                   folder_names;
	QString                       game_patch; // absolute path; empty when none
	QList<QPair<QString, QString>> environment;
};

namespace PerformanceProfiles {

// Configuration::performance_profile values besides a profile id.
inline constexpr char AUTOMATIC[] = "auto";
inline constexpr char NONE[]      = "none";

// The valid profiles in the launcher's profiles directory, sorted by name; read once.
[[nodiscard]] const QList<PerformanceProfile>& All();
[[nodiscard]] const PerformanceProfile*        Find(const QString& id);
// The profile a launch applies: the configured one, or for AUTOMATIC the first profile whose
// title ids hold the game's title id, then the first whose folder names match its folder.
[[nodiscard]] const PerformanceProfile* Resolve(const Configuration& info);

} // namespace PerformanceProfiles

#endif // PERFORMANCE_PROFILE_H
