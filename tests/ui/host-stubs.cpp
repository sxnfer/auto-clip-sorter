#include "host-stubs.hpp"
#include "settings-store.hpp"
#include "ui-thread.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include <QApplication>
#include <QFile>
#include <QMap>
#include <QMetaObject>

extern "C" const char *obs_module_text(const char *key)
{
	static const auto strings = [] {
		QMap<QByteArray, QByteArray> result;
		QFile file(QString::fromUtf8(CLIP_ATLAS_LOCALE));
		if (file.open(QIODevice::ReadOnly)) {
			for (const auto &line : file.readAll().split('\n')) {
				const int equals = line.indexOf('=');
				if (equals > 0)
					result.insert(line.left(equals),
						      line.mid(equals + 1).trimmed().mid(1).chopped(1));
			}
		}
		return result;
	}();
	const auto found = strings.constFind(key);
	return found == strings.cend() ? key : found->constData();
}

extern "C" void obs_log(int, const char *, ...) {}

config_t *obs_frontend_get_profile_config()
{
	return nullptr;
}

extern "C" bool config_get_bool(config_t *, const char *, const char *)
{
	return false;
}

extern "C" const char *config_get_string(config_t *, const char *, const char *)
{
	return nullptr;
}

namespace clip_atlas {
std::string test_clips_folder;

const Settings &SettingsStore::get() const
{
	return settings_;
}

std::string SettingsStore::effective_clips_folder() const
{
	return test_clips_folder;
}

void run_on_ui(std::function<void()> fn)
{
	QMetaObject::invokeMethod(qApp, std::move(fn), Qt::AutoConnection);
}
} // namespace clip_atlas
