#include "netplay_host_catalog.h"
#include "mod_plugins.h"
#include "ape_netplay_profile_contract.h"
#include <string.h>

/* This is title qualification metadata, not a gameplay modification. The
 * shared runtime owns CPU detection, actual-image identity, overlay sealing,
 * wire exchange, selection/confirmation and the before-tick0 barrier. */
int ape_netplay_host_profiles(const PSXModNetplayHostStartup *in,
                             PSXHostProfileCatalog *out) {
    if (out) { out->rules=0;out->count=0; }
#if !defined(_WIN32)
    (void)in;return 0; /* Native other-platform equivalence is not qualified. */
#else
    static PSXHostProfileRule rule;
    if (!in || !out || in->cpu.architecture!=PSX_HOST_ARCH_X64 ||
        in->fp_contract!=2 || in->compiled_backends!=PSX_HOST_BACKEND_ENHANCED ||
        in->available_backends!=PSX_HOST_BACKEND_ENHANCED ||
        !in->interpreted_overlays_sealed || !in->builtin_linked_bios ||
        !in->actual_executable_sha256 || !in->builtin_bios_image_id ||
        strcmp(in->builtin_bios_image_id,"OPENBIOS") ||
        in->builtin_bios_image_wordsum!=0xC4DAEB88u ||
        in->slot_count!=2 || (in->player_count && in->player_count!=2) ||
        (in->occupied_mask && in->occupied_mask!=3) || !in->rollback ||
        in->loading_enabled || in->observer || in->aspect_index ||
        (in->renderer!=0 && in->renderer!=1)) return 0;
    if (!rule.revision) {
        const char *hex=APE_NETPLAY_QUALIFICATION_SHA256;
        strcpy(rule.profile,"ape-openbios-x64-v1");
        rule.priority=1;rule.flags=PSX_HOST_PROFILE_PORTABLE;
        rule.implementation_count=1;
        rule.implementations[0].backend=PSX_HOST_BACKEND_ENHANCED;
        rule.implementations[0].architectures=1u<<PSX_HOST_ARCH_X64;
        for (unsigned i=0;i<32;++i) {
            unsigned value=0;
            for (unsigned j=0;j<2;++j) {
                unsigned c=(unsigned char)hex[2*i+j];
                if(c>='0'&&c<='9')c-='0';
                else if(c>='a'&&c<='f')c=c-'a'+10;
                else return 0;
                value=(value<<4)|c;
            }
            rule.qualification[i]=(unsigned char)value;
        }
        rule.revision=1;
    }
    out->rules=&rule;out->count=1;return 1;
#endif
}

PSX_MOD_CONSTRUCTOR(ape_register_netplay_host_catalog) {
    (void)psx_mod_register_vanilla_host_profiles(ape_netplay_host_profiles);
}
