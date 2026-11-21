#include "netplay_host_catalog.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int ape_netplay_host_profiles(const PSXModNetplayHostStartup *,PSXHostProfileCatalog *);
static unsigned char image[32]={1};
static PSXModNetplayHostStartup valid(void) {
    PSXModNetplayHostStartup in={0};
    in.cpu.architecture=PSX_HOST_ARCH_X64;
    in.compiled_backends=in.available_backends=PSX_HOST_BACKEND_ENHANCED;
    in.fp_contract=2;in.interpreted_overlays_sealed=in.builtin_linked_bios=1;
    in.actual_executable_sha256=image;in.builtin_bios_image_id="OPENBIOS";
    in.builtin_bios_image_wordsum=0xC4DAEB88u;in.slot_count=2;in.rollback=1;
    return in;
}
int main(void) {
    PSXModNetplayHostStartup in=valid();PSXHostProfileCatalog cat;
    assert(!psx_mod_netplay_profile());
    assert(psx_mod_netplay_host_profiles()==ape_netplay_host_profiles);
#if defined(_WIN32)
    assert(ape_netplay_host_profiles(&in,&cat) && cat.count==1);
    assert(cat.rules[0].flags==PSX_HOST_PROFILE_PORTABLE);
    assert(cat.rules[0].implementations[0].required_features==0);
    PSXHostCapabilities caps;
    assert(psx_host_caps_init(&caps,&in.cpu,2,2,image));
    assert(psx_host_caps_offer(&caps,cat.rules,2));
    assert(psx_host_caps_validate(&caps,&cat));
    /* The portable offer remains valid without an optional SIMD feature. */
    in.cpu.features_known=1;in.cpu.features=PSX_HOST_SSE2;
    assert(ape_netplay_host_profiles(&in,&cat));
    in.renderer=1;assert(ape_netplay_host_profiles(&in,&cat));
    in.player_count=2;in.occupied_mask=3;assert(ape_netplay_host_profiles(&in,&cat));
#define REFUSE(field,value) do {in=valid();in.field=value;cat.count=99; \
    assert(!ape_netplay_host_profiles(&in,&cat) && !cat.count && !cat.rules);}while(0)
    REFUSE(cpu.architecture,PSX_HOST_ARCH_ARM64);
    REFUSE(fp_contract,1);REFUSE(compiled_backends,1);REFUSE(available_backends,0);
    REFUSE(interpreted_overlays_sealed,0);REFUSE(builtin_linked_bios,0);
    REFUSE(actual_executable_sha256,NULL);REFUSE(builtin_bios_image_id,"SCPH1001");
    REFUSE(builtin_bios_image_wordsum,0);REFUSE(slot_count,3);REFUSE(player_count,1);
    REFUSE(occupied_mask,1);REFUSE(rollback,0);REFUSE(loading_enabled,1);
    REFUSE(observer,1);REFUSE(aspect_index,1);REFUSE(renderer,2);
#else
    assert(!ape_netplay_host_profiles(&in,&cat) && !cat.count && !cat.rules);
#endif
    puts("Ape host policy: PASS (candidate metadata; native qualification is separate)");
}
