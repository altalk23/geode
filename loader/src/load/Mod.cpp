#include <about.hpp>
#include <Geode/loader/Hook.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/loader/Log.hpp>
#include <Geode/loader/Mod.hpp>
#include <Geode/loader/Interface.hpp>
#include <Geode/loader/Setting.hpp>
#include <Geode/utils/conststring.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/JsonValidation.hpp>
#include <Geode/utils/map.hpp>
#include <Geode/utils/string.hpp>
#include <Geode/utils/vector.hpp>
#include <InternalMod.hpp>
#include <ZipUtils.h>

USE_GEODE_NAMESPACE();

nlohmann::json& DataStore::operator[](std::string const& key) {
    return m_mod->m_dataStore[key];
}

DataStore& DataStore::operator=(nlohmann::json& jsn) {
    m_mod->m_dataStore = jsn;
    return *this;
}

nlohmann::json& DataStore::getJson() const {
    return m_mod->m_dataStore;
}

bool DataStore::contains(std::string const& key) const {
    return m_mod->m_dataStore.contains(key);
}

DataStore::operator nlohmann::json() {
    return m_mod->m_dataStore;
}

DataStore::~DataStore() {
    if (m_store != m_mod->m_dataStore) {
        m_mod->postDSUpdate();
    }
}

Mod::Mod(ModInfo const& info) {
    this->m_info = info;
}

Mod::~Mod() {
    this->unload();
}

Result<> Mod::loadSettings() {
    // settings

    // Check if settings exist
    auto settPath = m_saveDirPath / "settings.json";
    if (ghc::filesystem::exists(settPath)) {
        auto settData = file_utils::readString(settPath);
        if (!settData) return settData;
        try {
            // parse settings.json
            auto data = nlohmann::json::parse(settData.value());
            JsonChecker checker(data);
            auto root = checker.root("[settings.json]");

            for (auto& [key, value] : root.items()) {
                // check if this is a known setting
                if (auto sett = this->getSetting(key)) {
                    // load its value
                    if (!sett->load(value.json())) {
                        return Err(
                            "Unable to load value for setting \"" +
                            key + "\""
                        );
                    }
                } else {
                    this->logInfo(
                        "Encountered unknown setting \"" + key + "\" while "
                        "loading settings",
                        Severity::Warning
                    );
                }
            }

        } catch(std::exception& e) {
            return Err(std::string("Unable to parse settings: ") + e.what());
        }
    }

    // datastore
    auto dsPath = m_saveDirPath / "ds.json";
    if (!ghc::filesystem::exists(dsPath)) {
        m_dataStore = m_info.m_defaultDataStore;
    } else {
        auto dsData = file_utils::readString(dsPath);
        if (!dsData) return dsData;
        try {
            m_dataStore = nlohmann::json::parse(dsData.value());
        } catch(std::exception& e) {
            return Err(std::string("Unable to parse datastore: ") + e.what());
        }
    }
    return Ok();
}

Result<> Mod::saveSettings() {
    // settings
    auto settPath = m_saveDirPath / "settings.json";
    auto json = nlohmann::json::object();
    for (auto& [key, sett] : m_info.m_settings) {
        if (!sett->save(json[key])) {
            return Err("Unable to save setting \"" + key + "\"");
        }
    }
    auto sw = file_utils::writeString(settPath, json.dump(4));
    if (!sw) {
        return sw;
    }

    // datastore
    auto dsPath = m_saveDirPath / "ds.json";
    auto dw = file_utils::writeString(dsPath, m_dataStore.dump(4));
    if (!dw) {
        return dw;
    }

    return Ok();
}

DataStore Mod::getDataStore() {
    return DataStore(this, m_dataStore);
}

void Mod::postDSUpdate() {
    /*EventCenter::get()->send(Event(
        "datastore-changed",
        this
    ), this);*/
    // FIXME: Dispatch
}

Result<> Mod::createTempDir() {
    ZipFile unzip(this->m_info.m_path.string());

    if (!unzip.isLoaded()) {
        return Err<>("Unable to unzip " + this->m_info.m_path.string());
    }

    if (!unzip.fileExists(this->m_info.m_binaryName)) {
        return Err<>(
            "Unable to find platform binary under the name \"" +
            this->m_info.m_binaryName + "\""
        );
    }

    auto tempDir = Loader::get()->getGeodeDirectory() / GEODE_TEMP_DIRECTORY;
    if (!ghc::filesystem::exists(tempDir)) {
        if (!ghc::filesystem::create_directory(tempDir)) {
            return Err<>("Unable to create temp directory for mods!");
        }
    }
    
    auto tempPath = ghc::filesystem::path(tempDir) / this->m_info.m_id;
    if (!ghc::filesystem::exists(tempPath) && !ghc::filesystem::create_directories(tempPath)) {
        return Err<>("Unable to create temp directory");
    }
    m_tempDirName = tempPath;

    for (auto file : unzip.getAllFiles()) {
        auto path = ghc::filesystem::path(file);
        if (path.has_parent_path()) {
            if (
                !ghc::filesystem::exists(tempPath / path.parent_path()) &&
                !ghc::filesystem::create_directories(tempPath / path.parent_path())
            ) {
                return Err<>("Unable to create directories \"" + path.parent_path().string() + "\"");
            }
        }
        unsigned long size;
        auto data = unzip.getFileData(file, &size);
        if (!data || !size) {
            return Err<>("Unable to read \"" + std::string(file) + "\"");
        }
        auto wrt = file_utils::writeBinary(
            tempPath / file,
            byte_array(data, data + size)
        );
        if (!wrt) return Err<>("Unable to write \"" + file + "\": " + wrt.error());
    }

    this->m_addResourcesToSearchPath = true;

    return Ok<>(tempPath);
}

Result<> Mod::load() {
	if (this->m_loaded) {
        return Ok<>();
    }
    #define RETURN_LOAD_ERR(str) \
        {m_loadErrorInfo = str; \
        return Err<>(m_loadErrorInfo);}

    if (!this->m_tempDirName.string().size()) {
        auto err = this->createTempDir();
        if (!err) RETURN_LOAD_ERR("Unable to create temp directory: " + err.error());
    }

    if (this->hasUnresolvedDependencies()) {
        RETURN_LOAD_ERR("Mod has unresolved dependencies");
    }
    auto err = this->loadPlatformBinary();
    if (!err) RETURN_LOAD_ERR(err.error());
    if (this->m_implicitLoadFunc) {
        auto r = this->m_implicitLoadFunc(this);
        if (!r) {
            this->unloadPlatformBinary();
            RETURN_LOAD_ERR("Implicit mod entry point returned an error");
        }
    }
    if (this->m_loadFunc) {
        auto r = this->m_loadFunc(this);
        if (!r) {
            this->unloadPlatformBinary();
            RETURN_LOAD_ERR("Mod entry point returned an error");
        }
    }
    this->m_loaded = true;
    if (this->m_loadDataFunc) {
        if (!this->m_loadDataFunc(this->m_saveDirPath.string().c_str())) {
            this->logInfo("Mod load data function returned false", Severity::Error);
        }
    }
    m_loadErrorInfo = "";
    Loader::get()->updateAllDependencies();
    return Ok<>();
}

Result<> Mod::unload() {
    if (!this->m_loaded) {
        return Ok<>();
    }

    if (!m_info.m_supportsUnloading) {
        return Err<>("Mod does not support unloading");
    }
    
    if (this->m_saveDataFunc) {
        if (!this->m_saveDataFunc(this->m_saveDirPath.string().c_str())) {
            this->logInfo("Mod save data function returned false", Severity::Error);
        }
    }

    if (this->m_unloadFunc) {
        this->m_unloadFunc();
    }

    for (auto const& hook : this->m_hooks) {
        auto d = this->disableHook(hook);
        if (!d) return d;
        delete hook;
    }
    this->m_hooks.clear();

    for (auto const& patch : this->m_patches) {
        if (!patch->restore()) {
            return Err<>("Unable to restore patch at " + std::to_string(patch->getAddress()));
        }
        delete patch;
    }
    this->m_patches.clear();

    auto res = this->unloadPlatformBinary();
    if (!res) {
        return res;
    }
    this->m_loaded = false;
    Loader::get()->updateAllDependencies();
    return Ok<>();
}

Result<> Mod::enable() {
    if (!this->m_loaded) {
        return Err<>("Mod is not loaded");
    }
    
    if (this->m_enableFunc) {
        if (!this->m_enableFunc()) {
            return Err<>("Mod enable function returned false");
        }
    }

    for (auto const& hook : this->m_hooks) {
        auto d = this->enableHook(hook);
        if (!d) return d;
    }

    for (auto const& patch : this->m_patches) {
        if (!patch->apply()) {
            return Err<>("Unable to apply patch at " + std::to_string(patch->getAddress()));
        }
    }

    this->m_enabled = true;

    return Ok<>();
}

Result<> Mod::disable() {
    if (!m_info.m_supportsDisabling) {
        return Err<>("Mod does not support disabling");
    }

    if (this->m_disableFunc) {
        if (!this->m_disableFunc()) {
            return Err<>("Mod disable function returned false");
        }
    }

    for (auto const& hook : this->m_hooks) {
        auto d = this->disableHook(hook);
        if (!d) return d;
    }

    for (auto const& patch : this->m_patches) {
        if (!patch->restore()) {
            return Err<>("Unable to restore patch at " + std::to_string(patch->getAddress()));
        }
    }

    this->m_enabled = false;

    return Ok<>();
}

Result<> Mod::uninstall() {
    if (m_info.m_supportsDisabling) {
        auto r = this->disable();
        if (!r) return r;
        if (m_info.m_supportsUnloading) {
            auto ur = this->unload();
            if (!ur) return ur;
        }
    }
    if (!ghc::filesystem::remove(m_info.m_path)) {
        return Err<>(
            "Unable to delete mod's .geode file! "
            "This might be due to insufficient permissions - "
            "try running GD as administrator."
        );
    }
    return Ok<>();
}

bool Mod::isUninstalled() const {
    return this != InternalMod::get() && !ghc::filesystem::exists(m_info.m_path);
}

bool Dependency::isUnresolved() const {
    return this->m_required &&
           (this->m_state == ModResolveState::Unloaded ||
           this->m_state == ModResolveState::Unresolved ||
           this->m_state == ModResolveState::Disabled);
}

bool Mod::updateDependencyStates() {
    bool hasUnresolved = false;
	for (auto & dep : this->m_info.m_dependencies) {
		if (!dep.m_mod) {
			dep.m_mod = Loader::get()->getLoadedMod(dep.m_id);
		}
		if (dep.m_mod) {
			dep.m_mod->updateDependencyStates();

			if (dep.m_mod->hasUnresolvedDependencies()) {
				dep.m_state = ModResolveState::Unresolved;
			} else {
				if (!dep.m_mod->m_resolved) {
					dep.m_mod->m_resolved = true;
					dep.m_state = ModResolveState::Resolved;
                    auto r = dep.m_mod->load();
                    if (!r) {
                        dep.m_state = ModResolveState::Unloaded;
                        dep.m_mod->logInfo(r.error(), Severity::Error);
                    }
                    else {
                    	auto r = dep.m_mod->enable();
                    	if (!r) {
	                        dep.m_state = ModResolveState::Disabled;
	                        dep.m_mod->logInfo(r.error(), Severity::Error);
	                    }
                    }
				} else {
					if (dep.m_mod->isEnabled()) {
						dep.m_state = ModResolveState::Loaded;
					} else {
						dep.m_state = ModResolveState::Disabled;
					}
				}
			}
		} else {
			dep.m_state = ModResolveState::Unloaded;
		}
		if (dep.isUnresolved()) {
			this->m_resolved = false;
            this->unload();
            hasUnresolved = true;
		}
	}
    if (!hasUnresolved && !this->m_resolved) {
        Log::get() << Severity::Debug << "All dependencies for " << m_info.m_id << " found";
        this->m_resolved = true;
        if (this->m_enabled) {
            Log::get() << Severity::Debug << "Resolved & loading " << m_info.m_id;
            auto r = this->load();
            if (!r) {
                Log::get() << Severity::Error << this << "Error loading: " << r.error();
            }
            else {
            	auto r = this->enable();
	            if (!r) {
	                Log::get() << Severity::Error << this << "Error enabling: " << r.error();
	            }
            }
        } else {
            Log::get() << Severity::Debug << "Resolved " << m_info.m_id << ", however not loading it as it is disabled";
        }
    }
    return hasUnresolved;
}

bool Mod::hasUnresolvedDependencies() const {
	for (auto const& dep : this->m_info.m_dependencies) {
		if (dep.isUnresolved()) {
			return true;
		}
	}
	return false;
}

std::vector<Dependency> Mod::getUnresolvedDependencies() {
    std::vector<Dependency> res;
	for (auto const& dep : this->m_info.m_dependencies) {
		if (dep.isUnresolved()) {
			res.push_back(dep);
		}
	}
    return res;
}

ghc::filesystem::path Mod::getSaveDir() const {
    return this->m_saveDirPath;
}

decltype(ModInfo::m_id) Mod::getID() const {
    return this->m_info.m_id;
}

decltype(ModInfo::m_name) Mod::getName() const {
    return this->m_info.m_name;
}

decltype(ModInfo::m_developer) Mod::getDeveloper() const {
    return this->m_info.m_developer;
}

decltype(ModInfo::m_description) Mod::getDescription() const {
    return this->m_info.m_description;
}

decltype(ModInfo::m_details) Mod::getDetails() const {
    return this->m_info.m_details;
}

ModInfo Mod::getModInfo() const {
    return m_info;
}

ghc::filesystem::path Mod::getTempDir() const {
    return m_tempDirName;
}

ghc::filesystem::path Mod::getBinaryPath() const {
    return m_tempDirName / m_info.m_binaryName;
}

std::string Mod::getPath() const {
    return this->m_info.m_path.string();
}

VersionInfo Mod::getVersion() const {
    return this->m_info.m_version;
}

bool Mod::isEnabled() const {
    return this->m_enabled;
}

bool Mod::isLoaded() const {
    return this->m_loaded;
}

bool Mod::supportsDisabling() const {
    return this->m_info.m_supportsDisabling;
}

bool Mod::supportsUnloading() const {
    return this->m_info.m_supportsUnloading;
}

bool Mod::wasSuccesfullyLoaded() const {
    return !this->isEnabled() || this->isLoaded();
}

std::vector<Hook*> Mod::getHooks() const {
    return this->m_hooks;
}

Log Mod::log() {
    return Log(this);
}

void Mod::logInfo(
    std::string const& info,
    Severity severity
) {
    Log l(this);
    l << severity << info;
}

bool Mod::depends(std::string const& id) const {
    return vector_utils::contains<Dependency>(
        this->m_info.m_dependencies,
        [id](Dependency t) -> bool { return t.m_id == id; }
    );
}

const char* Mod::expandSpriteName(const char* name) {
    static std::unordered_map<std::string, const char*> expanded = {};
    if (expanded.count(name)) {
        return expanded[name];
    }
    auto exp = new char[strlen(name) + 2 + this->m_info.m_id.size()];
    auto exps = this->m_info.m_id + "/" + name;
    memcpy(exp, exps.c_str(), exps.size() + 1);
    expanded[name] = exp;
    return exp;
}

bool Mod::hasSettings() const {
    return m_info.m_settings.size();
}

decltype(ModInfo::m_settings) Mod::getSettings() const {
    return m_info.m_settings;
}

std::shared_ptr<Setting> Mod::getSetting(std::string const& key) const {
    for (auto& sett : m_info.m_settings) {
        if (sett.first == key) {
            return sett.second;
        }
    }
    return nullptr;
}

bool Mod::hasSetting(std::string const& key) const {
    for (auto& sett : m_info.m_settings) {
        if (sett.first == key) {
            return true;
        }
    }
    return false;
}

std::string Mod::getLoadErrorInfo() const {
    return m_loadErrorInfo;
}
