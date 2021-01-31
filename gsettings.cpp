#define WLR_USE_UNSTABLE
#define WAYFIRE_PLUGIN
#include <gio/gio.h>
#include <unistd.h>

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <wayfire/config-backend.hpp>
#include <wayfire/config/file.hpp>
#include <wayfire/core.hpp>
#include <wayfire/debug.hpp>
#include <wayfire/output.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/util/log.hpp>

struct conf_change {
	std::string sec;
	std::string key;
	GVariant *val;
};

static std::unordered_map<std::string, GSettings *> gsets;
static std::unordered_map<GSettings *, std::string> gsets_rev;
static std::queue<conf_change> changes;
static std::mutex init_mtx;
static std::condition_variable init_cv;
static bool init_done = false;

static void gsettings_callback(GSettings *settings, gchar *key, gpointer user_data) {
	int fd = (int)(intptr_t)user_data;
	std::string skey(key);
	changes.push(conf_change{gsets_rev[settings], skey, g_settings_get_value(settings, key)});
	if (init_done) {
		write(fd, "!", 1);
		char buff;
		read(fd, &buff, 1);
	}
}

static void gsettings_update_schemas(int fd) {
	LOGD("Updating schemas");
	for (auto sec : wf::get_core().config.get_all_sections()) {
		std::optional<std::string> reloc_path;
		auto sec_name = sec->get_name();
		if (gsets.count(sec_name) != 0) {
			LOGD("Skipping existing section ", sec_name);
			continue;
		}
		auto schema_name = "org.wayfire.section." + sec_name;
		size_t splitter = sec_name.find_first_of(":");
		if (splitter != std::string::npos) {
			auto obj_type_name = sec_name.substr(0, splitter);  // e.g. 'core.output'
			auto section_name = sec_name.substr(splitter + 1);
			if (!obj_type_name.empty() && !section_name.empty()) {
				schema_name = "org.wayfire.section." + obj_type_name;
				std::replace(obj_type_name.begin(), obj_type_name.end(), '.', '/');
				reloc_path = "/org/wayfire/section/" + obj_type_name + "/" + section_name + "/";
				LOGD("Adding section ", sec_name, " relocatable schema ", schema_name, " at path ",
				     *reloc_path);
			} else {
				LOGD("Adding section ", sec_name,
				     " has ':' but could not split name, continuing as fixed schema ", schema_name);
			}
		} else {
			LOGD("Adding section ", sec_name, " fixed schema ", schema_name);
		}
		g_autoptr(GSettingsSchema) schema = g_settings_schema_source_lookup(
		    g_settings_schema_source_get_default(), schema_name.c_str(), TRUE);
		if (!schema) {
			LOGE("GSettings schema not found: ", schema_name.c_str(), " ",
			     reloc_path ? reloc_path->c_str() : "");
			continue;
		}
		auto is_reloc = g_settings_schema_get_path(schema) == nullptr;
		if (!reloc_path && is_reloc) {
			continue;
		}
		GSettings *gs = nullptr;
		if (reloc_path)
			gs = g_settings_new_with_path(schema_name.c_str(), reloc_path->c_str());
		else
			gs = g_settings_new(schema_name.c_str());
		if (!gs) {
			LOGE("GSettings object not found: ", schema_name.c_str(), " ",
			     reloc_path ? reloc_path->c_str() : "");
			continue;
		}
		gsets.emplace(sec_name, gs);
		gsets_rev.emplace(gs, sec_name);
		// For future changes
		g_signal_connect(gsets[sec_name], "changed", G_CALLBACK(gsettings_callback),
		                 (void *)(uintptr_t)fd);
		// Initial values
		gchar **keys = g_settings_schema_list_keys(schema);
		while (*keys != nullptr) {
			gsettings_callback(gs, *keys++, (void *)(uintptr_t)fd);
		}
	}
}

static void gsettings_meta_callback(GSettings *settings, gchar *key, gpointer user_data) {
	int fd = (int)(intptr_t)user_data;
	std::string skey(key);
	if (skey == "dyn-sections") {
		LOGD("Updating dynamic sections");
		size_t lstlen = 0;
		const gchar **lst = g_variant_get_strv(g_settings_get_value(settings, key), &lstlen);
		for (size_t i = 0; i < lstlen; i++) {
			std::string sec(lst[i]);  // e.g. 'core.output:eDP-1' - member of dyn-sections
			if (!wf::get_core().config.get_section(sec)) {
				LOGI("Adding dynamic section ", sec);
				size_t splitter = sec.find_first_of(":");
				auto obj_type_name = sec.substr(0, splitter);  // e.g. 'core.output'
				auto parent_section = wf::get_core().config.get_section(obj_type_name);
				if (!parent_section) {
					LOGE("No parent section ", obj_type_name, " for relocatable ", sec);
					continue;
				}
				wf::get_core().config.merge_section(parent_section->clone_with_name(sec));
			}
		}
		g_free(lst);
		gsettings_update_schemas(fd);
	}
}

static void gsettings_loop(int fd) {
	usleep(100000);
	auto *gctx = g_main_context_new();
	g_main_context_push_thread_default(gctx);
	auto *loop = g_main_loop_new(gctx, false);
	GSettings *mgs = g_settings_new("org.wayfire.gsettings");
	if (!mgs) {
		LOGE("GSettings object org.wayfire.gsettings not found - relocatable functionality lost!");
	} else {
		// For future changes
		g_signal_connect(mgs, "changed", G_CALLBACK(gsettings_meta_callback), (void *)(uintptr_t)fd);
		// Initial values
		g_autoptr(GSettingsSchema) schema = nullptr;
		g_object_get(mgs, "settings-schema", &schema, NULL);
		gchar **keys = g_settings_schema_list_keys(schema);
		while (*keys != nullptr) {
			gsettings_meta_callback(mgs, *keys++, (void *)(uintptr_t)fd);
		}
	}
	gsettings_update_schemas(fd);
	{
		std::lock_guard<std::mutex> lk(init_mtx);
		init_done = true;
		init_cv.notify_all();
	}
	g_main_loop_run(loop);
}

static int handle_update(int fd, uint32_t /* mask */, void *data);
class wayfire_gsettings;
static void apply_update(const wayfire_gsettings *ctx);

class wayfire_gsettings : public wf::config_backend_t {
 public:
	std::thread loopthread;
	int fd[2] = {0, 0};
	wf::wl_timer sig_debounce;
	wf::config::config_manager_t *config;

	void init(wl_display *display, wf::config::config_manager_t &config,
	          const std::string &) override {
		this->config = &config;
		config = wf::config::build_configuration(get_xml_dirs(), "", "");

		pipe(fd);
		loopthread = std::thread(gsettings_loop, fd[1]);
		wl_event_loop_add_fd(wl_display_get_event_loop(display), fd[0], WL_EVENT_READABLE,
		                     handle_update, this);
		{
			std::unique_lock<std::mutex> lk(init_mtx);
			init_cv.wait(lk, [] { return init_done; });
			apply_update(this);
		}
	}

	void load_settings() {}
};

static void apply_field(const wayfire_gsettings *ctx, GVariant *val, const std::string &sec,
                        const std::string &key) {
	const auto *typ = g_variant_get_type(val);
	auto opt = ctx->config->get_section(sec)->get_option_or(key);
	if (!opt) {
		LOGW("GSettings update found non-existent option: ", sec.c_str(), "/", key.c_str());
		return;
	}
	if (g_variant_type_equal(typ, G_VARIANT_TYPE_STRING)) {
		std::string str_val(g_variant_get_string(val, NULL));
		opt->set_value_str(str_val);
	} else if (g_variant_type_equal(typ, G_VARIANT_TYPE_BOOLEAN)) {
		auto topt = std::dynamic_pointer_cast<wf::config::option_t<bool>>(opt);
		if (topt == nullptr) {
			LOGW("GSettings update could not cast opt to bool: ", sec.c_str(), "/", key.c_str());
		} else {
			topt->set_value(g_variant_get_boolean(val));
		}
	} else if (g_variant_type_equal(typ, G_VARIANT_TYPE_INT32)) {
		auto topt = std::dynamic_pointer_cast<wf::config::option_t<int>>(opt);
		if (topt == nullptr) {
			LOGW("GSettings update could not cast opt to int: ", sec.c_str(), "/", key.c_str());
		} else {
			topt->set_value(g_variant_get_int32(val));
		}
	} else if (g_variant_type_equal(typ, G_VARIANT_TYPE_DOUBLE)) {
		auto topt = std::dynamic_pointer_cast<wf::config::option_t<double>>(opt);
		if (topt == nullptr) {
			LOGW("GSettings update could not cast opt to double: ", sec.c_str(), "/", key.c_str());
		} else {
			topt->set_value(static_cast<float>(g_variant_get_double(val)));
		}
	} else if (g_variant_type_equal(typ, G_VARIANT_TYPE("(dddd)"))) {
		wf::color_t color{static_cast<float>(g_variant_get_double(g_variant_get_child_value(val, 0))),
		                  static_cast<float>(g_variant_get_double(g_variant_get_child_value(val, 1))),
		                  static_cast<float>(g_variant_get_double(g_variant_get_child_value(val, 2))),
		                  static_cast<float>(g_variant_get_double(g_variant_get_child_value(val, 3)))};

		auto topt = std::dynamic_pointer_cast<wf::config::option_t<wf::color_t>>(opt);
		if (topt == nullptr) {
			LOGW("GSettings update could not cast opt to color: ", sec.c_str(), "/", key.c_str());
		} else {
			topt->set_value(color);
		}
	} else if (g_variant_type_is_array(typ)) {
		auto topt = std::dynamic_pointer_cast<wf::config::compound_option_t>(opt);
		if (topt == nullptr) {
			LOGW("GSettings update could not cast opt to dynamic-list: ", sec.c_str(), "/", key.c_str());
		} else {
			wf::config::compound_option_t::stored_type_t entries;
			gchar *entry_key = nullptr;
			GVariant *entry_val = nullptr;
			GVariantIter iter;
			g_variant_iter_init(&iter, val);
			while (g_variant_iter_loop(&iter, "{s*}", &entry_key, &entry_val)) {
				std::vector<std::string> &entry = entries.emplace_back();
				entry.push_back(std::string(entry_key));
				for (size_t i = 0; i < g_variant_n_children(entry_val); i++) {
					g_autoptr(GVariant) v = g_variant_get_child_value(entry_val, i);
					const auto *etyp = g_variant_get_type(v);
					if (g_variant_type_equal(etyp, G_VARIANT_TYPE_STRING)) {
						entry.push_back(std::string(g_variant_get_string(v, NULL)));
					} else if (g_variant_type_equal(etyp, G_VARIANT_TYPE_BOOLEAN)) {
						entry.push_back(g_variant_get_boolean(v) ? "1" : "0");
					} else if (g_variant_type_equal(etyp, G_VARIANT_TYPE_INT32)) {
						entry.push_back(std::to_string(g_variant_get_int32(v)));
					} else if (g_variant_type_equal(etyp, G_VARIANT_TYPE_DOUBLE)) {
						entry.push_back(std::to_string(g_variant_get_double(v)));
					} else {
						LOGI("GSettings update has unsupported type in dynamic-list: ", sec.c_str(), "/",
						     key.c_str(), " key: ", entry_key, " item idx: ", i);
					}
				}
			}
			if (!topt->set_value_untyped(entries)) {
				LOGW("GSettings failed to apply dynamic-list options: ", sec.c_str(), "/", key.c_str());
			}
		}
	} else {
		LOGI("GSettings update has unsupported type: ", sec.c_str(), "/", key.c_str());
	}
}

static void apply_update(const wayfire_gsettings *ctx) {
	while (!changes.empty()) {
		auto chg = changes.front();
		// GSettings does not support underscores
		std::replace(chg.key.begin(), chg.key.end(), '-', '_');
		try {
			apply_field(ctx, chg.val, chg.sec, chg.key);
		} catch (std::invalid_argument &e) {
			LOGE("GSettings update could not apply: ", chg.sec.c_str(), "/", chg.key.c_str(), ": ",
			     e.what());
		}
		g_variant_unref(chg.val);
		changes.pop();
	}
}

static int handle_update(int fd, uint32_t /* mask */, void *data) {
	auto *ctx = reinterpret_cast<wayfire_gsettings *>(data);
	char buff;
	read(fd, &buff, 1);
	apply_update(ctx);
	// The signal triggers relatively heavy stuff like cursor theme loading
	// Firing it per value is not the best idea
	// TODO: if possible, add more efficient way to wayfire, without readding source (UPD: return
	// true?)
	ctx->sig_debounce.disconnect();
	ctx->sig_debounce.set_timeout(69, []() {
		wf::get_core().emit_signal("reload-config", nullptr);
		LOGI("GSettings applied");
		return false;  // disconnect
	});
	write(fd, "!", 1);
	return 1;
}

DECLARE_WAYFIRE_CONFIG_BACKEND(wayfire_gsettings);
