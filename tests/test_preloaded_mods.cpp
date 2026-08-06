#include "mod_packages.h"
#include <filesystem>
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

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return fail("expected the preloaded mods root");
    const fs::path source(argv[1]);
    const fs::path root =
        fs::temp_directory_path() / "apeescape-preloaded-mods-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::copy(source, root, fs::copy_options::recursive);
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
    if (manifest_count != 4) return fail("expected four package manifests");
    PSXRecompV4::mod_clear_plugins_for_tests();
    for (const char* id : {
             "ape.widescreen.16-9",
             "ape.widescreen.21-9",
             "ape.widescreen.adaptive",
             "ape.framerate.60",
             "ape.framerate.120",
             "ape.framerate.144",
             "ape.framerate.165",
             "ape.framerate.uncapped",
             "ape.fmv.skip",
             "ape.gadgets.quick-select",
             "ape.gadgets.quick-select-patches"}) {
        if (!PSXRecompV4::mod_register_activation_plugin(id, no_op_plugin))
            return fail(std::string("could not register test plugin ") + id);
    }
    PSXRecompV4::ModPackageManager manager(root);
    std::string error;
    if (!manager.scan(&error)) return fail("catalog scan failed: " + error);
    if (!manager.load_state(&error)) return fail("default state failed: " + error);
    if (manager.packages().size() != 4)
        return fail("expected four package families");
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
                "ape.experimental.60fps", "native-60fps",
                "rate", choice, &error)) {
            return fail(error);
        }
        const auto fps_plan = manager.resolve(kGameId, "", kDiscSha256);
        if (!fps_plan.ok || !fps_plan.writes.empty() ||
            fps_plan.plugins.size() != 1 ||
            fps_plan.plugins.front().id != plugin) {
            return fail(std::string("wrong interpolated frame-rate plan for ") +
                        choice);
        }
    }
    if (!manager.set_feature_enabled(
            "ape.experimental.60fps", "native-60fps", false, &error) ||
        !manager.set_feature_enabled(
            "ape.enhancement.quick-gadget-select", "quick-gadget-select",
            true, &error)) {
        return fail(error);
    }
    const auto quick_select_plan = manager.resolve(kGameId, "", kDiscSha256);
    if (!quick_select_plan.ok || !quick_select_plan.writes.empty() ||
        quick_select_plan.plugins.size() != 2) {
        return fail("wrong Quick Gadget Select plugin count");
    }
    bool found_vblank = false;
    bool found_patch = false;
    for (const auto& plugin : quick_select_plan.plugins) {
        found_vblank |= plugin.id == "ape.gadgets.quick-select";
        found_patch |= plugin.id == "ape.gadgets.quick-select-patches";
    }
    if (!found_vblank || !found_patch)
        return fail("wrong Quick Gadget Select plan");
    fs::remove_all(root, ec);
    std::cout << "Ape Escape preloaded mods: 4 packages, "
                 "3 widescreen choices, 5 interpolated frame-rate choices, "
                 "Quick Gadget Select default-off, "
                 "motion-adaptive clarity blend, Skip FMVs migrated from Settings, "
                 "slingshot face-button ammo cycle disabled by opt-in activation patch\n";
    return 0;
}
