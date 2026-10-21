// Clip Atlas — settings.json: loading, saving and telling listeners about changes.
//
// Saving goes through obs_data_save_json_safe(): the new JSON is written to a temporary file and then
// renamed over settings.json, so a crash mid-write never leaves a half-written settings.json. Every
// public function is UI thread only (see settings-store.hpp).
#include "settings-store.hpp"

#include <obs-data.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace clip_atlas {

namespace {

constexpr const char *kSettingsFile = "settings.json";
constexpr const char *kTempExt = "tmp";
constexpr const char *kBackupExt = "bak";
constexpr const char *kBadSuffix = ".bad";

// Owners for the strings and references OBS hands out. Each one releases on scope exit, including
// when an exception unwinds. A null value is fine, since unique_ptr does not call its deleter for null.
using BmemString = std::unique_ptr<char, void (*)(void *)>;
using DataPtr = std::unique_ptr<obs_data_t, void (*)(obs_data_t *)>;
using ArrayPtr = std::unique_ptr<obs_data_array_t, void (*)(obs_data_array_t *)>;

// Holds one reference to an obs_data_item_t. obs_data_item_next() moves that reference along the
// item list, so the slot is handed out rather than a copy of the pointer.
class ItemRef {
public:
	explicit ItemRef(obs_data_item_t *item) : item_(item) {}
	~ItemRef() { obs_data_item_release(&item_); }
	ItemRef(const ItemRef &) = delete;
	ItemRef &operator=(const ItemRef &) = delete;

	obs_data_item_t *get() const { return item_; }
	obs_data_item_t **slot() { return &item_; }

private:
	obs_data_item_t *item_;
};

// Whether `name` is set in `data` to a value of `type`. A missing key gives false.
bool holds(obs_data_t *data, const char *name, enum obs_data_type type)
{
	ItemRef item(obs_data_item_byname(data, name));
	return item.get() && obs_data_item_gettype(item.get()) == type;
}

// Integers only. OBS also stores JSON reals as numbers, and those are rejected so that a value like
// 4.5 keeps its default instead of being truncated.
bool holds_int(obs_data_t *data, const char *name)
{
	ItemRef item(obs_data_item_byname(data, name));
	return item.get() && obs_data_item_gettype(item.get()) == OBS_DATA_NUMBER &&
	       obs_data_item_numtype(item.get()) == OBS_DATA_NUM_INT;
}

// The string under `name`, or "" when there is none.
std::string string_value(obs_data_t *data, const char *name)
{
	const char *value = obs_data_get_string(data, name);
	return value ? value : "";
}

// Every string field of a record object becomes an entry. Fields of other types are skipped, so one
// bad field cannot cost the rest of the record.
SettingsRecord read_record(obs_data_t *object)
{
	SettingsRecord record;
	ItemRef item(obs_data_first(object));
	while (item.get()) {
		const char *name = obs_data_item_get_name(item.get());
		if (name && obs_data_item_gettype(item.get()) == OBS_DATA_STRING)
			record[name] = string_value(object, name);
		obs_data_item_next(item.slot());
	}
	return record;
}

// Reads from an obs_data_t that the caller keeps alive for as long as the reader is used. A key that is
// missing or of another type reports nullopt, so load_settings() keeps that field's default.
class ObsDataReader final : public SettingsReader {
public:
	explicit ObsDataReader(obs_data_t *data) : data_(data) {}

	std::optional<bool> get_bool(const std::string &key) const override
	{
		if (!holds(data_, key.c_str(), OBS_DATA_BOOLEAN))
			return std::nullopt;
		return obs_data_get_bool(data_, key.c_str());
	}

	std::optional<long long> get_int(const std::string &key) const override
	{
		if (!holds_int(data_, key.c_str()))
			return std::nullopt;
		return obs_data_get_int(data_, key.c_str());
	}

	std::optional<std::string> get_string(const std::string &key) const override
	{
		if (!holds(data_, key.c_str(), OBS_DATA_STRING))
			return std::nullopt;
		return string_value(data_, key.c_str());
	}

	// obs_data arrays can only hold objects, so each element is {"value": "<string>"}. Other elements
	// are skipped.
	std::optional<std::vector<std::string>> get_string_list(const std::string &key) const override
	{
		ArrayPtr array = array_at(key);
		if (!array)
			return std::nullopt;
		std::vector<std::string> values;
		const size_t count = obs_data_array_count(array.get());
		for (size_t i = 0; i < count; ++i) {
			DataPtr element(obs_data_array_item(array.get(), i), obs_data_release);
			if (element && holds(element.get(), "value", OBS_DATA_STRING))
				values.push_back(string_value(element.get(), "value"));
		}
		return values;
	}

	std::optional<std::vector<SettingsRecord>> get_records(const std::string &key) const override
	{
		ArrayPtr array = array_at(key);
		if (!array)
			return std::nullopt;
		std::vector<SettingsRecord> records;
		const size_t count = obs_data_array_count(array.get());
		for (size_t i = 0; i < count; ++i) {
			DataPtr element(obs_data_array_item(array.get(), i), obs_data_release);
			if (element)
				records.push_back(read_record(element.get()));
		}
		return records;
	}

private:
	// A new reference to the array under `key`, or a null pointer if there is no array there.
	ArrayPtr array_at(const std::string &key) const
	{
		if (!holds(data_, key.c_str(), OBS_DATA_ARRAY))
			return ArrayPtr(nullptr, obs_data_array_release);
		return ArrayPtr(obs_data_get_array(data_, key.c_str()), obs_data_array_release);
	}

	obs_data_t *data_;
};

// Writes into an obs_data_t that the caller then saves.
class ObsDataWriter final : public SettingsWriter {
public:
	explicit ObsDataWriter(obs_data_t *data) : data_(data) {}

	void set_bool(const std::string &key, bool value) override { obs_data_set_bool(data_, key.c_str(), value); }

	void set_int(const std::string &key, long long value) override { obs_data_set_int(data_, key.c_str(), value); }

	void set_string(const std::string &key, const std::string &value) override
	{
		obs_data_set_string(data_, key.c_str(), value.c_str());
	}

	// Each string is wrapped as {"value": "..."}, since obs_data arrays can only hold objects.
	void set_string_list(const std::string &key, const std::vector<std::string> &value) override
	{
		ArrayPtr array(obs_data_array_create(), obs_data_array_release);
		for (const std::string &item : value) {
			DataPtr object(obs_data_create(), obs_data_release);
			obs_data_set_string(object.get(), "value", item.c_str());
			// The array keeps its own reference, so the local one is released at scope exit.
			obs_data_array_push_back(array.get(), object.get());
		}
		// The parent keeps its own reference to the array as well.
		obs_data_set_array(data_, key.c_str(), array.get());
	}

	void set_records(const std::string &key, const std::vector<SettingsRecord> &value) override
	{
		ArrayPtr array(obs_data_array_create(), obs_data_array_release);
		for (const SettingsRecord &record : value) {
			DataPtr object(obs_data_create(), obs_data_release);
			for (const auto &[field, text] : record)
				obs_data_set_string(object.get(), field.c_str(), text.c_str());
			obs_data_array_push_back(array.get(), object.get());
		}
		obs_data_set_array(data_, key.c_str(), array.get());
	}

private:
	obs_data_t *data_;
};

// Serializes `settings` and writes settings.json. Logs and returns false on any failure.
bool write_settings_file(const Settings &settings)
{
	BmemString folder(obs_module_config_path(""), bfree);
	BmemString path(obs_module_config_path(kSettingsFile), bfree);
	if (!folder || !path) {
		obs_log(LOG_ERROR, "no config folder, settings not saved");
		return false;
	}
	// The folder is created on the first save, so a fresh install needs no setup step.
	if (os_mkdirs(folder.get()) == MKDIR_ERROR) {
		obs_log(LOG_ERROR, "could not create %s, settings not saved", folder.get());
		return false;
	}
	try {
		DataPtr data(obs_data_create(), obs_data_release);
		ObsDataWriter writer(data.get());
		save_settings(settings, writer);
		if (!obs_data_save_json_safe(data.get(), path.get(), kTempExt, kBackupExt)) {
			obs_log(LOG_ERROR, "could not write %s, settings not saved", path.get());
			return false;
		}
		return true;
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "could not save settings: %s", e.what());
		return false;
	}
}

// Calls each subscribed listener in ascending id order, which is subscription order. The map is copied
// first, so a listener may subscribe or unsubscribe during the pass. A listener removed earlier in the
// pass is skipped, because its owner may already be gone. Each call gets its own copy of the settings,
// so a listener that calls set() cannot change the value it is reading.
void notify_listeners(std::map<int, SettingsStore::Listener> &listeners, const Settings &settings)
{
	try {
		const std::map<int, SettingsStore::Listener> snapshot = listeners;
		for (const auto &[id, listener] : snapshot) {
			if (listeners.find(id) == listeners.end())
				continue;
			const Settings current = settings;
			try {
				listener(current);
			} catch (const std::exception &e) {
				obs_log(LOG_ERROR, "settings listener %d failed: %s", id, e.what());
			} catch (...) {
				obs_log(LOG_ERROR, "settings listener %d failed: unknown exception", id);
			}
		}
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "could not notify settings listeners: %s", e.what());
	}
}

} // namespace

void SettingsStore::load()
{
	try {
		BmemString path(obs_module_config_path(kSettingsFile), bfree);
		if (!path) {
			obs_log(LOG_WARNING, "no config folder, using default settings");
			return;
		}
		if (!os_file_exists(path.get())) {
			obs_log(LOG_INFO, "no %s yet, using default settings", kSettingsFile);
			return;
		}
		DataPtr data(obs_data_create_from_json_file(path.get()), obs_data_release);
		if (!data) {
			// Moved aside, not deleted, so the user can see what was wrong. An older .bad is replaced.
			const std::string bad = std::string(path.get()) + kBadSuffix;
			if (os_rename(path.get(), bad.c_str()) == 0) {
				obs_log(LOG_WARNING, "%s could not be read; moved to %s, using default settings",
					path.get(), bad.c_str());
			} else {
				obs_log(LOG_WARNING,
					"%s could not be read and could not be moved to %s, using default settings",
					path.get(), bad.c_str());
			}
			return;
		}
		ObsDataReader reader(data.get());
		settings_ = load_settings(reader);
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "loading settings failed, using defaults: %s", e.what());
	}
}

const Settings &SettingsStore::get() const
{
	return settings_;
}

bool SettingsStore::set(const Settings &settings)
{
	settings_ = validated(settings);
	const bool saved = write_settings_file(settings_);
	notify_listeners(listeners_, settings_);
	return saved;
}

int SettingsStore::subscribe(Listener listener)
{
	const int id = next_listener_id_++;
	listeners_.emplace(id, std::move(listener));
	return id;
}

void SettingsStore::unsubscribe(int id)
{
	auto found = listeners_.find(id);
	if (found == listeners_.end())
		return;
	// Moved out before the erase, so a listener whose destructor calls back into the store cannot
	// change the map while it is being erased.
	Listener removed = std::move(found->second);
	listeners_.erase(found);
}

std::string SettingsStore::effective_clips_folder() const
{
	if (!settings_.clips_folder.empty())
		return settings_.clips_folder;
	BmemString recording(obs_frontend_get_current_record_output_path(), bfree);
	return recording ? recording.get() : "";
}

SettingsStore &settings_store()
{
	// Constructed on first use, so no static-initialization order to worry about.
	static SettingsStore store;
	return store;
}

} // namespace clip_atlas
