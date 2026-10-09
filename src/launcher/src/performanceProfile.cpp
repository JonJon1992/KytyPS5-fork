#include "performanceProfile.h"

#include "configuration.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <utility>

namespace PerformanceProfiles {

static QStringList ReadStrings(const QJsonObject& root, const char* key) {
	QStringList ret;
	for (const auto& value: root.value(QLatin1String(key)).toArray()) {
		const auto text = value.toString().trimmed();
		if (!text.isEmpty()) {
			ret << text;
		}
	}
	return ret;
}

// Same rules as the bundled preset (main.cpp): only KYTY_*/TRACY_* keys with string values.
static bool ReadProfile(const QString& path, PerformanceProfile* profile) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		qWarning("Performance profile %s: cannot open", qUtf8Printable(path));
		return false;
	}
	QJsonParseError error;
	const auto      document = QJsonDocument::fromJson(file.readAll(), &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		qWarning("Performance profile %s: invalid JSON (%s)", qUtf8Printable(path),
		         qUtf8Printable(error.errorString()));
		return false;
	}
	const auto root = document.object();
	profile->id     = QFileInfo(path).completeBaseName();
	profile->name   = root.value(QStringLiteral("name")).toString().trimmed();
	if (profile->name.isEmpty()) {
		profile->name = profile->id;
	}
	profile->notes        = root.value(QStringLiteral("notes")).toString().trimmed();
	profile->title_ids    = ReadStrings(root, "title_ids");
	profile->folder_names = ReadStrings(root, "folder_names");
	const auto patch      = root.value(QStringLiteral("game_patch")).toString().trimmed();
	if (!patch.isEmpty()) {
		profile->game_patch =
		    QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(patch);
	}
	const auto environment = root.value(QStringLiteral("environment")).toObject();
	for (auto it = environment.begin(); it != environment.end(); ++it) {
		if ((!it.key().startsWith(QStringLiteral("KYTY_")) &&
		     !it.key().startsWith(QStringLiteral("TRACY_"))) ||
		    !it.value().isString()) {
			qWarning("Performance profile %s: invalid entry %s", qUtf8Printable(path),
			         qUtf8Printable(it.key()));
			return false;
		}
		profile->environment.append({it.key(), it.value().toString()});
	}
	return true;
}

const QList<PerformanceProfile>& All() {
	static const QList<PerformanceProfile> profiles = [] {
		QList<PerformanceProfile> ret;
		const QDir dir(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("profiles")));
		for (const auto& entry: dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
			PerformanceProfile profile;
			if (ReadProfile(entry.absoluteFilePath(), &profile)) {
				ret.append(std::move(profile));
			}
		}
		std::stable_sort(ret.begin(), ret.end(), [](const auto& a, const auto& b) {
			return QString::localeAwareCompare(a.name, b.name) < 0;
		});
		return ret;
	}();
	return profiles;
}

const PerformanceProfile* Find(const QString& id) {
	const auto& profiles = All();
	const auto  it = std::find_if(profiles.begin(), profiles.end(),
	                              [&id](const auto& profile) { return profile.id == id; });
	return it != profiles.end() ? &*it : nullptr;
}

const PerformanceProfile* Resolve(const Configuration& info) {
	const auto& selected = info.performance_profile;
	if (selected == QLatin1String(NONE)) {
		return nullptr;
	}
	if (!selected.isEmpty() && selected != QLatin1String(AUTOMATIC)) {
		return Find(selected);
	}
	const auto title_id = info.title_id.trimmed();
	if (!title_id.isEmpty()) {
		for (const auto& profile: All()) {
			if (profile.title_ids.contains(title_id, Qt::CaseInsensitive)) {
				return &profile;
			}
		}
	}
	const auto folder = QFileInfo(QDir::cleanPath(info.basedir)).fileName();
	if (!folder.isEmpty()) {
		for (const auto& profile: All()) {
			for (const auto& name: profile.folder_names) {
				if (folder.contains(name, Qt::CaseInsensitive)) {
					return &profile;
				}
			}
		}
	}
	return nullptr;
}

} // namespace PerformanceProfiles
