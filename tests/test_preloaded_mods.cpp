#include "mod_packages.h"
#include "mod_media.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

// These bundled packages contain no donor media. Fail explicitly if a fixture
// unexpectedly starts depending on it, rather than opening external game data.
namespace PSXRecompV4 {
bool load_mod_media(const fs::path&, const std::string&, uint64_t,
                    const std::string&,
                    std::shared_ptr<const std::vector<uint8_t>>&,
                    std::string* error) {
    if (error) *error = "donor media is outside this catalog test";
    return false;
}
}

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
    if (argc != 3) return fail("expected preloaded and development mods roots");

    const fs::path source(argv[1]);
    {
        PSXRecompV4::ModPackage capture;
        std::string error;
        if (!PSXRecompV4::ModPackageManager::read_manifest(
                fs::path(argv[2]) / "packages/ape.development.scene-capture/1.0.0/manifest.toml",
                capture, &error)) return fail("capture manifest: " + error);
        if (capture.features.size() != 1 || capture.features.front().default_enabled ||
            capture.features.front().channel != PSXRecompV4::ModChannel::Developer)
            return fail("capture must remain developer-only and default-off");
    }
    const fs::path root =
        fs::temp_directory_path() / "apeescape-preloaded-mods-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::copy(source, root, fs::copy_options::recursive);
    if (fs::exists(source / "packages/ape.enhancement.frame-smoothing"))
        return fail("frame smoothing must stay outside the active catalog");
    if (fs::exists(fs::path(argv[2]) / "packages/ape.enhancement.frame-smoothing"))
        return fail("retired frame smoothing must not remain in development");

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

    PSXRecompV4::mod_clear_plugins_for_tests();
    for (const char* id : {
             "ape.widescreen.16-9",
             "ape.widescreen.21-9",
             "ape.widescreen.adaptive",
             "ape.fmv.skip",
             "ape.gadgets.quick-select",
             "ape.fix.prop-visibility",
             "psx.pgxp"}) {
        if (!PSXRecompV4::mod_register_activation_plugin(id, no_op_plugin))
            return fail(std::string("could not register test plugin ") + id);
    }

    PSXRecompV4::ModPackageManager manager(root);
    std::string error;
    if (!manager.scan(&error)) return fail("catalog scan failed: " + error);
    if (!manager.load_state(&error)) return fail("default state failed: " + error);
    if (manager.packages().size() != 5)
        return fail("expected five package families");

    const auto default_plan = manager.resolve(kGameId, "", kDiscSha256);
    if (!default_plan.ok || !default_plan.writes.empty() ||
        default_plan.plugins.size() != 4 ||
        default_plan.plugins.front().id != "ape.fmv.skip" ||
        default_plan.plugins[1].id != "ape.widescreen.adaptive" ||
        default_plan.plugins[2].id != "ape.fix.prop-visibility" ||
        default_plan.plugins.back().id != "psx.pgxp") {
        return fail("default catalog did not enable Skip FMVs, Fit to screen, prop visibility and PGXP");
    }
    if (!manager.set_feature_enabled(
            "ape.enhancement.skip-fmvs", "skip-fmvs", false, &error) ||
        !manager.set_feature_enabled(
            "psx.enhancement.pgxp", "pgxp", false, &error)) {
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
        if (!plan.ok || plan.plugins.size() != 2 ||
            plan.plugins.front().id != plugin ||
            plan.plugins.back().id != "ape.fix.prop-visibility") {
            return fail(std::string("wrong widescreen plugin for ") + choice);
        }
    }

    if (!manager.set_feature_enabled(
            "ape.enhancement.widescreen", "widescreen", false, &error))
        return fail(error);
    if (manager.set_feature_enabled("ape.enhancement.frame-smoothing",
            "temporal-blending", true, &error))
        return fail("retired interpolation unexpectedly resolved");
    if (!manager.set_feature_enabled("ape.enhancement.quick-gadget-select",
            "quick-gadget-select", true, &error)) return fail(error);

    /*
     * Quick Gadget Select is one vblank plugin plus one main_exe instruction
     * patch. That patch has to be a declarative write rather than a
     * psx_mod_write_code_word() from an activation callback: activation runs
     * before the guest boots, so the game's own EXE load would overwrite it.
     * Asserting the write is planned is what tells the two apart -- a
     * plugin-count assertion passes either way while the patch does nothing.
     */
    const auto quick_select_plan = manager.resolve(kGameId, "", kDiscSha256);
    if (!quick_select_plan.ok || quick_select_plan.plugins.size() != 2 ||
        quick_select_plan.plugins.front().id != "ape.gadgets.quick-select" ||
        quick_select_plan.plugins.back().id != "ape.fix.prop-visibility") {
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
    if (!disabled_plan.ok || !disabled_plan.writes.empty() ||
        disabled_plan.plugins.size() != 1 ||
        disabled_plan.plugins.front().id != "ape.fix.prop-visibility")
        return fail("prop correction must remain enabled independently of enhancements");

    fs::remove_all(root, ec);
    std::cout << "Ape Escape preloaded mods: 5 packages, default PGXP and prop visibility, "
                 "3 widescreen choices, retired interpolation absent, "
                 "Skip FMVs migrated from Settings, "
                 "Quick Gadget Select default-off with a declarative "
                 "slingshot-block patch, stock guest code untouched by default\n";
    return 0;
}
