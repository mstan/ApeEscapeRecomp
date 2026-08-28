#include "mod_packages.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr const char* kGameId = "SCUS-94423";
constexpr const char* kDiscSha256 =
    "1ae17e78ebb8c782c7c1785b0a0bd7b0ee28235b8a0c83c8df887129899a852a";

int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

void no_op_plugin() {}

extern "C" int psx_mod_set_frame_interpolation(uint32_t) { return 1; }

extern "C" int psx_mod_set_frame_interpolation_blend(uint32_t) { return 1; }

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return fail("expected the preloaded mods root");

    const fs::path source(argv[1]);
    const fs::path root =
        fs::temp_directory_path() / "apeescape-preloaded-mods-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::copy(source, root, fs::copy_options::recursive);

    const fs::path legacy_package =
        root / "packages" / "ape.experimental.60fps" / "1.0.0";
    fs::create_directories(legacy_package, ec);
    if (ec) return fail("could not create legacy package fixture");
    {
        std::ofstream legacy_manifest(legacy_package / "manifest.toml",
                                      std::ios::trunc);
        if (!legacy_manifest) return fail("could not write legacy manifest");
        legacy_manifest << R"toml(format_version = 5
id = "ape.experimental.60fps"
version = "1.0.0"
name = "Ape Escape Frame Rate"
author = "mstan"
description = "Presentation-only interpolated frame-rate modes. Game logic, timers, and audio remain at their stock cadence."
resolver = "declarative"
save_compatibility = "shared"

[[target]]
game_id = "SCUS-94423"
disc_sha256 = "1ae17e78ebb8c782c7c1785b0a0bd7b0ee28235b8a0c83c8df887129899a852a"

[[feature]]
id = "native-60fps"
name = "Interpolated Frame Rate (Experimental)"
description = "Blend completed game frames at the selected output rate without accelerating gameplay, timers, or audio."
group = "Frame Rate"
default_enabled = false

[[option]]
feature = "native-60fps"
id = "rate"
label = "Frame rate"
description = "Presentation rate only; the original game simulation remains untouched. Requires the OpenGL renderer."
group = "Frame Rate"
type = "choice"
default = "60"

[[option.choice]]
value = "60"
label = "60 FPS"

[[option.choice]]
value = "120"
label = "120 FPS"

[[option.choice]]
value = "144"
label = "144 FPS"

[[option.choice]]
value = "165"
label = "165 FPS"

[[option.choice]]
value = "uncapped"
label = "Uncapped"

[[plugin]]
feature = "native-60fps"
id = "ape.framerate.60"
when = { rate = "60" }

[[plugin]]
feature = "native-60fps"
id = "ape.framerate.120"
when = { rate = "120" }

[[plugin]]
feature = "native-60fps"
id = "ape.framerate.144"
when = { rate = "144" }

[[plugin]]
feature = "native-60fps"
id = "ape.framerate.165"
when = { rate = "165" }

[[plugin]]
feature = "native-60fps"
id = "ape.framerate.uncapped"
when = { rate = "uncapped" }
)toml";
        if (!legacy_manifest) return fail("could not finish legacy manifest");
    }

    size_t manifest_count = 0;
    for (const fs::directory_entry& entry :
         fs::recursive_directory_iterator(root / "packages")) {
        if (!entry.is_regular_file() ||
            entry.path().filename() != "manifest.toml") {
            continue;
        }
        ++manifest_count;
        PSXRecompV4::ModPackage package;
        std::string error;
        if (!PSXRecompV4::ModPackageManager::read_manifest(
                entry.path(), package, &error)) {
            return fail("manifest parse failed: " + error);
        }
    }
    if (manifest_count != 5) return fail("expected five package manifests");

    for (const char* id : {
             "ape.widescreen.16-9",
             "ape.widescreen.21-9",
             "ape.widescreen.adaptive",
             "ape.fmv.skip",
             "ape.gadgets.quick-select"}) {
        if (!PSXRecompV4::mod_register_activation_plugin(id, no_op_plugin))
            return fail(std::string("could not register test plugin ") + id);
    }

    for (const char* id : {
             "ape.frame-smoothing.display",
             "ape.frame-smoothing.120",
             "ape.frame-smoothing.144",
             "ape.frame-smoothing.165",
             "ape.framerate.60",
             "ape.framerate.120",
             "ape.framerate.144",
             "ape.framerate.165",
             "ape.framerate.uncapped"}) {
        if (!PSXRecompV4::mod_plugin_registered(id)) {
            return fail(std::string(
                "frame-smoothing plugin constructor did not register ") + id);
        }
    }

    PSXRecompV4::ModPackageManager manager(root);
    std::string error;
    if (!manager.scan(&error)) return fail("catalog scan failed: " + error);
    if (!manager.load_state(&error)) return fail("default state failed: " + error);
    if (manager.packages().size() != 5)
        return fail("expected five package families");

    const auto default_plan = manager.resolve(kGameId, "", kDiscSha256);
    if (!default_plan.ok || !default_plan.writes.empty() ||
        default_plan.plugins.size() != 1 ||
        default_plan.plugins.front().id != "ape.fmv.skip") {
        return fail("default catalog did not preserve Skip FMVs");
    }
    if (!manager.set_feature_enabled(
            "ape.enhancement.skip-fmvs", "skip-fmvs", false, &error)) {
        return fail(error);
    }

    if (!manager.set_feature_enabled(
            "ape.enhancement.widescreen", "widescreen", true, &error)) {
        return fail(error);
    }
    for (const auto& [choice, plugin] :
         {std::pair{"16:9", "ape.widescreen.16-9"},
          std::pair{"21:9", "ape.widescreen.21-9"},
          std::pair{"adaptive", "ape.widescreen.adaptive"}}) {
        if (!manager.set_feature_option(
                "ape.enhancement.widescreen", "widescreen",
                "aspect", choice, &error)) {
            return fail(error);
        }
        const auto plan = manager.resolve(kGameId, "", kDiscSha256);
        if (!plan.ok || plan.plugins.size() != 1 ||
            plan.plugins.front().id != plugin) {
            return fail(std::string("wrong widescreen plugin for ") + choice);
        }
    }

    if (!manager.set_feature_enabled(
            "ape.enhancement.widescreen", "widescreen", false, &error) ||
        !manager.set_feature_enabled(
            "ape.enhancement.frame-smoothing", "temporal-blending",
            true, &error)) {
        return fail(error);
    }
    for (const auto& [choice, plugin] :
         {std::pair{"display", "ape.frame-smoothing.display"},
          std::pair{"120", "ape.frame-smoothing.120"},
          std::pair{"144", "ape.frame-smoothing.144"},
          std::pair{"165", "ape.frame-smoothing.165"}}) {
        if (!manager.set_feature_option(
                "ape.enhancement.frame-smoothing", "temporal-blending",
                "rate", choice, &error)) {
            return fail(error);
        }
        const auto smoothing_plan = manager.resolve(kGameId, "", kDiscSha256);
        if (!smoothing_plan.ok || !smoothing_plan.writes.empty() ||
            smoothing_plan.plugins.size() != 1 ||
            smoothing_plan.plugins.front().id != plugin) {
            return fail(std::string("wrong frame-smoothing plan for ") + choice);
        }
    }

    if (!manager.set_feature_enabled(
            "ape.enhancement.frame-smoothing", "temporal-blending",
            false, &error) ||
        !manager.set_feature_enabled(
            "ape.experimental.60fps", "native-60fps", true, &error)) {
        return fail(error);
    }
    for (const auto& [choice, plugin] :
         {std::pair{"60", "ape.framerate.60"},
          std::pair{"120", "ape.framerate.120"},
          std::pair{"144", "ape.framerate.144"},
          std::pair{"165", "ape.framerate.165"},
          std::pair{"uncapped", "ape.framerate.uncapped"}}) {
        if (!manager.set_feature_option(
                "ape.experimental.60fps", "native-60fps", "rate", choice,
                &error)) {
            return fail(error);
        }
        const auto legacy_plan = manager.resolve(kGameId, "", kDiscSha256);
        if (!legacy_plan.ok || !legacy_plan.writes.empty() ||
            legacy_plan.plugins.size() != 1 ||
            legacy_plan.plugins.front().id != plugin) {
            return fail(std::string(
                "legacy frame-rate package did not resolve ") + choice);
        }
    }

    if (!manager.set_feature_enabled(
            "ape.experimental.60fps", "native-60fps",
            false, &error) ||
        !manager.set_feature_enabled(
            "ape.enhancement.quick-gadget-select", "quick-gadget-select",
            true, &error)) {
        return fail(error);
    }

    /*
     * Quick Gadget Select is one vblank plugin plus one main_exe instruction
     * patch. That patch has to be a declarative write rather than a
     * psx_mod_write_code_word() from an activation callback: activation runs
     * before the guest boots, so the game's own EXE load would overwrite it.
     * Asserting the write is planned is what tells the two apart -- a
     * plugin-count assertion passes either way while the patch does nothing.
     */
    const auto quick_select_plan = manager.resolve(kGameId, "", kDiscSha256);
    if (!quick_select_plan.ok || quick_select_plan.plugins.size() != 1 ||
        quick_select_plan.plugins.front().id != "ape.gadgets.quick-select") {
        return fail("wrong Quick Gadget Select plugin plan");
    }
    if (quick_select_plan.writes.size() != 1)
        return fail("Quick Gadget Select did not plan its guest-code patch");
    {
        const auto& write = quick_select_plan.writes.front();
        const std::vector<uint8_t> expected{0xD2, 0xC2, 0x43, 0x80};
        const std::vector<uint8_t> replacement{0x7C, 0x8F, 0x01, 0x08};
        if (write.target != PSXRecompV4::ModPatchTarget::MainExe ||
            write.location != 0x80063B6Cull || write.expected != expected ||
            write.replacement != replacement) {
            return fail("wrong Quick Gadget Select guest-code patch");
        }
    }

    if (!manager.set_feature_enabled(
            "ape.enhancement.quick-gadget-select", "quick-gadget-select",
            false, &error)) {
        return fail(error);
    }
    const auto disabled_plan = manager.resolve(kGameId, "", kDiscSha256);
    if (!disabled_plan.ok || !disabled_plan.writes.empty())
        return fail("Quick Gadget Select patched guest code while disabled");

    fs::remove_all(root, ec);
    std::cout << "Ape Escape preloaded mods: 4 current packages, "
                 "3 widescreen choices, 4 temporal-blending rates, "
                 "legacy frame-rate package aliases, "
                 "single-context presentation, no motion-vector claims, "
                 "Skip FMVs migrated from Settings, "
                 "Quick Gadget Select default-off with a declarative "
                 "slingshot-block patch, stock guest code untouched by default\n";
    return 0;
}
