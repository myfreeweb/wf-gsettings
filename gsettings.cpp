#define WLR_USE_UNSTABLE
#define WAYFIRE_PLUGIN
#include <gio/gio.h>
#include <unistd.h>

#include <queue>
#include <thread>
#include <unordered_map>
#include <wayfire/core.hpp>
#include <wayfire/debug.hpp>
#include <wayfire/output.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/config-backend.hpp>
#include <wayfire/util/log.hpp>
#include <wayfire/config/file.hpp>

struct conf_change {
	std::string sec;
	std::string key;
	GVariant *val;
};

static std::unordered_map<std::string, GSettings *> gsets;
static std::unordered_map<GSettings *, std::string> gsets_rev;
static std::queue<conf_change> changes;

static void gsettings_callback(GSettings *settings, gchar *key, gpointer user_data) {
	int fd = (int)(intptr_t)user_data;
	std::string skey(key);
	changes.push(conf_change{gsets_rev[settings], skey, g_settings_get_value(settings, key)});
	write(fd, "!", 1);
	char buff;
	read(fd, &buff, 1);
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
	// XXX: not really safe to read the list from this secondary thread
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
	g_main_loop_run(loop);
}

static int handle_update(int fd, uint32_t /* mask */, void *data);

class wayfire_gsettings : public wf::config_backend_t
{
  public:
	std::thread loopthread;
	int fd[2] = {0, 0};
	wf::wl_timer sig_debounce;
	wf::config::config_manager_t *config;

	void init(wl_display *display, wf::config::config_manager_t& config,
		const std::string&) override
	{
		this->config = &config;
		config = wf::config::build_configuration(
			get_xml_dirs(), "", "");

		pipe(fd);
		loopthread = std::thread(gsettings_loop, fd[1]);
		wl_event_loop_add_fd(wl_display_get_event_loop(display),
			fd[0], WL_EVENT_READABLE, handle_update, this);
	}

    void load_settings() {
    }
};

static int handle_update(int fd, uint32_t /* mask */, void *data) {
	auto *ctx = reinterpret_cast<wayfire_gsettings *>(data);
	char buff;
	read(fd, &buff, 1);
	while (!changes.empty()) {
		auto chg = changes.front();
		// GSettings does not support underscores
		std::replace(chg.key.begin(), chg.key.end(), '-', '_');
		try {
			auto opt = ctx->config->get_section(chg.sec)->get_option(chg.key);
			const auto *typ = g_variant_get_type(chg.val);
			if (opt == nullptr) {
				LOGI("GSettings update found nullptr opt: ", chg.sec.c_str(), "/", chg.key.c_str());
			} else if (g_variant_type_equal(typ, G_VARIANT_TYPE_STRING)) {
				opt->set_value_str(std::string(g_variant_get_string(chg.val, NULL)));
			} else if (g_variant_type_equal(typ, G_VARIANT_TYPE_BOOLEAN)) {
				auto topt = std::dynamic_pointer_cast<wf::config::option_t<bool>>(opt);
				if (topt == nullptr) {
					LOGW("GSettings update could not cast opt to bool: ", chg.sec.c_str(), "/",
					     chg.key.c_str());
				} else {
					topt->set_value(g_variant_get_boolean(chg.val));
				}
			} else if (g_variant_type_equal(typ, G_VARIANT_TYPE_INT32)) {
				auto topt = std::dynamic_pointer_cast<wf::config::option_t<int>>(opt);
				if (topt == nullptr) {
					LOGW("GSettings update could not cast opt to int: ", chg.sec.c_str(), "/",
					     chg.key.c_str());
				} else {
					topt->set_value(g_variant_get_int32(chg.val));
				}
			} else if (g_variant_type_equal(typ, G_VARIANT_TYPE_DOUBLE)) {
				auto topt = std::dynamic_pointer_cast<wf::config::option_t<double>>(opt);
				if (topt == nullptr) {
					LOGW("GSettings update could not cast opt to double: ", chg.sec.c_str(), "/",
					     chg.key.c_str());
				} else {
					topt->set_value(static_cast<float>(g_variant_get_double(chg.val)));
				}
			} else if (g_variant_type_equal(typ, G_VARIANT_TYPE("(dddd)"))) {
				auto topt = std::dynamic_pointer_cast<wf::config::option_t<wf::color_t>>(opt);
				if (topt == nullptr) {
					LOGW("GSettings update could not cast opt to color: ", chg.sec.c_str(), "/",
					     chg.key.c_str());
				} else {
					topt->set_value(wf::color_t{
					    static_cast<float>(g_variant_get_double(g_variant_get_child_value(chg.val, 0))),
					    static_cast<float>(g_variant_get_double(g_variant_get_child_value(chg.val, 1))),
					    static_cast<float>(g_variant_get_double(g_variant_get_child_value(chg.val, 2))),
					    static_cast<float>(g_variant_get_double(g_variant_get_child_value(chg.val, 3)))});
				}
			} else {
				LOGI("GSettings update has unsupported type: ", chg.sec.c_str(), "/", chg.key.c_str());
			}
		} catch (std::invalid_argument &e) {
			LOGE("GSettings update could not apply: ", chg.sec.c_str(), "/", chg.key.c_str());
		}
		g_variant_unref(chg.val);
		changes.pop();
	}
	// The signal triggers relatively heavy stuff like cursor theme loading
	// Firing it per value e.g. when initially applying everything is a bad idea
	// TODO: if possible, add more efficient way to wayfire, without readding source
	ctx->sig_debounce.disconnect();
	ctx->sig_debounce.set_timeout(69, []() {
		wf::get_core().emit_signal("reload-config", nullptr);
		LOGI("GSettings applied");
	});
	write(fd, "!", 1);
	return 1;
}

DECLARE_WAYFIRE_CONFIG_BACKEND(wayfire_gsettings);
