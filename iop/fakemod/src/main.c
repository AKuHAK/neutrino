#include <loadcore.h>
#include <stdio.h>
#include <sysclib.h>
#include <xmodload.h>

#include "fakemod.h"
#include "ioplib.h"
#include "elf.h"
#include "mprintf.h"

#define MODNAME "fakemod"
IRX_ID(MODNAME, 1, 1);

struct fakemod_data fmd = {MODULE_SETTINGS_MAGIC};

// MODLOAD's exports pointers
static int (*org_LoadStartModule)(char *modpath, int arg_len, char *args, int *modres);
static int (*org_StartModule)(int id, char *modname, int arg_len, char *args, int *modres);
static int (*org_LoadModuleBuffer)(void *ptr);
static int (*org_StopModule)(int id, int arg_len, char *args, int *modres);
static int (*org_UnloadModule)(int id);
static int (*org_SearchModuleByName)(const char *modname);
static int (*org_ReferModuleStatus)(int mid, ModuleStatus *status);

#if 0 //def DEBUG // Too much text output, enable when needed
//--------------------------------------------------------------
static void print_libs()
{
    ModuleInfo_t *m = GetLoadcoreInternalData()->image_info;
    M_DEBUG("Module list:\n");
    M_DEBUG("  name                   |    start |   text |  data |   bss\n");
    while (m != NULL) {
        M_DEBUG("  %-22s | 0x%6x | %6d | %5d | %5d\n", m->name, m->text_start, m->text_size, m->data_size, m->bss_size);
        m = m->next;
    }
}
#else
static inline void print_libs() {}
#endif

#ifdef DEBUG
//--------------------------------------------------------------
static void print_args(int arg_len, char *args)
{
    // Multiple null terminated strings together
    int args_idx = 0;
    int was_null = 1;

    if (arg_len == 0)
        return;

    M_DEBUG("Module arguments (arg_len=%d):\n", arg_len);

    // Search strings
    while(args_idx < arg_len) {
        if (args[args_idx] == 0) {
            if (was_null == 1) {
                M_DEBUG("- args[%d]=0\n", args_idx);
            }
            was_null = 1;
        }
        else if (was_null == 1) {
            M_DEBUG("- args[%d]='%s'\n", args_idx, &args[args_idx]);
            was_null = 0;
        }
        args_idx++;
    }
}
#else
static inline void print_args(int arg_len, char *args) {}
#endif

//--------------------------------------------------------------
static struct FakeModule *checkFakemodByFile(const char *path, struct FakeModule *fakemod_list)
{
    // check if module is in the list
    while (fakemod_list->fname != NULL) {
        if (strstr(path, fakemod_list->fname)) {
            return fakemod_list;
        }
        fakemod_list++;
    }

    return NULL;
}

//--------------------------------------------------------------
static struct FakeModule *checkFakemodByName(const char *modname, struct FakeModule *fakemod_list)
{
    // check if module is in the list
    while (fakemod_list->fname != NULL) {
        if (strstr(modname, fakemod_list->name)) {
            return fakemod_list;
        }
        fakemod_list++;
    }

    return NULL;
}

//--------------------------------------------------------------
static struct FakeModule *checkFakemodById(int id, struct FakeModule *fakemod_list)
{
    // check if module is in the list
    while (fakemod_list->fname != NULL) {
        if (id == fakemod_list->id) {
            return fakemod_list;
        }
        fakemod_list++;
    }

    return NULL;
}

//--------------------------------------------------------------
static int Hook_LoadStartModule(char *modpath, int arg_len, char *args, int *modres)
{
    struct FakeModule *mod;

    print_libs();
    M_DEBUG("%s(%s, %d, ...)\n", __FUNCTION__, modpath, arg_len);
    print_args(arg_len, args);

    mod = checkFakemodByFile(modpath, fmd.fake);
    if (mod != NULL) {
        int rv;

        if (mod->returnLoad == 0) {
            // Fake module succesfully started
            *modres = mod->returnStart;
            rv = mod->id;
        }
        else {
            // Fake module load error
            rv = mod->returnLoad;
        }

        M_DEBUG("- FAKING! id=0x%x, rv=%d(0x%x), modres=%d\n", mod->id, rv, rv, *modres);
        return rv;
    }

    return org_LoadStartModule(modpath, arg_len, args, modres);
}

//--------------------------------------------------------------
static int Hook_StartModule(int id, char *modname, int arg_len, char *args, int *modres)
{
    struct FakeModule *mod;

    M_DEBUG("%s(0x%x, %s, %d, ...)\n", __FUNCTION__, id, modname, arg_len);
    print_args(arg_len, args);

    mod = checkFakemodById(id, fmd.fake);
    if (mod != NULL) {
        int rv;

        if (mod->returnLoad == 0) {
            // Fake module succesfully started
            *modres = mod->returnStart;
            rv = mod->id;
        }
        else {
            // Fake cannot start a module that is not loaded
            rv = -202; // KE_UNKNOWN_MODULE
        }

        M_DEBUG("- FAKING! id=0x%x, rv=%d(0x%x), modres=%d\n", mod->id, rv, rv, *modres);
        return rv;
    }

    return org_StartModule(id, modname, arg_len, args, modres);
}

//--------------------------------------------------------------
static int Hook_LoadModuleBuffer(void *ptr)
{
    struct FakeModule *mod;
    elf_header_t *eh = (elf_header_t *)ptr;
    elf_pheader_t *eph = (elf_pheader_t *)(ptr + eh->phoff);
    const char *modname = (const char *)ptr + eph->offset + 0x1a;

    print_libs();
    M_DEBUG("%s(0x%x) modname = '%s'\n", __FUNCTION__, ptr, modname);

    mod = checkFakemodByName(modname, fmd.fake);
    if (mod != NULL) {
        int rv;

        if (mod->returnLoad == 0) {
            // Fake module succesfully started
            rv = mod->id;
        }
        else {
            // Fake module load error
            rv = mod->returnLoad;
        }

        M_DEBUG("- FAKING! id=0x%x, rv=%d(0x%x)\n", mod->id, rv, rv);
        return rv;
    }

    return org_LoadModuleBuffer(ptr);
}

//--------------------------------------------------------------
static int Hook_StopModule(int id, int arg_len, char *args, int *modres)
{
    struct FakeModule *mod;

    M_DEBUG("%s(0x%x, %d, ...)\n", __FUNCTION__, id, arg_len);
    print_args(arg_len, args);

    mod = checkFakemodById(id, fmd.fake);
    if (mod != NULL) {
        M_DEBUG("- FAKING! id=0x%x\n", mod->id);

        if ((mod->prop & FAKE_PROP_UNLOAD) == 0)
            *modres = MODULE_NO_RESIDENT_END;
        else
            org_StopModule(org_SearchModuleByName(mod->name), arg_len, args, modres);

        return mod->id;
    }

    return org_StopModule(id, arg_len, args, modres);
}

//--------------------------------------------------------------
static int Hook_UnloadModule(int id)
{
    struct FakeModule *mod;

    M_DEBUG("%s(0x%x)\n", __FUNCTION__, id);

    mod = checkFakemodById(id, fmd.fake);
    if (mod != NULL) {
        M_DEBUG("- FAKING! id=0x%x\n", mod->id);

        if ((mod->prop & FAKE_PROP_UNLOAD) != 0)
            org_UnloadModule(org_SearchModuleByName(mod->name));

        return mod->id;
    }

    return org_UnloadModule(id);
}

//--------------------------------------------------------------
static int Hook_SearchModuleByName(char *modname)
{
    struct FakeModule *mod;

    M_DEBUG("%s(%s)\n", __FUNCTION__, modname);

    mod = checkFakemodByName(modname, fmd.fake);
    if (mod != NULL) {
        int rv = mod->id;
        if (mod->returnLoad != 0 || mod->returnStart == MODULE_NO_RESIDENT_END)
            rv = -202; // KE_UNKNOWN_MODULE
        M_DEBUG("- FAKING! id=0x%x rv=%d(0x%x)\n", mod->id, rv, rv);
        return rv;
    }

    return org_SearchModuleByName(modname);
}

//--------------------------------------------------------------
static int Hook_ReferModuleStatus(int id, ModuleStatus *status)
{
    struct FakeModule *mod;

    //M_DEBUG("%s(0x%x, ...)\n", __FUNCTION__, id);

    mod = checkFakemodById(id, fmd.fake);
    if (mod != NULL && mod->returnLoad == 0) {
        //M_DEBUG("- FAKING! id=0x%x\n", mod->id);
        memset(status, 0, sizeof(ModuleStatus));
        strcpy(status->name, mod->name);
        status->version = mod->version;
        status->id = mod->id;
        return id;
    }

    return org_ReferModuleStatus(id, status);
}

//--------------------------------------------------------------
int _start(int argc, char **argv)
{
    int i;

    print_libs();

    // Change string index to string pointers
    M_DEBUG("Fake module list:\n");
    M_DEBUG("         fname | name           | vers. |  rl | rs | prop\n");
    for (i = 0; i < MODULE_SETTINGS_MAX_FAKE_COUNT; i++) {
        struct FakeModule *fm = &fmd.fake[i];

        // Transform file name index to pointer
        if ((unsigned int)fm->fname >= 0x80000000) {
            unsigned int idx = (unsigned int)fm->fname - 0x80000000;
            fm->fname = (const char *)&fmd.data[idx];
        }

        // Transform module name index to pointer
        if ((unsigned int)fm->name >= 0x80000000) {
            unsigned int idx = (unsigned int)fm->name - 0x80000000;
            fm->name = (const char *)&fmd.data[idx];
        }

        if (fm->fname != NULL) {
            M_DEBUG("  %12s | %-14s | 0x%3x | %3d | %2d | 0x%x\n", fm->fname, fm->name, fm->version, fm->returnLoad, fm->returnStart, fm->prop);
        }
    }

    iop_library_t * lib_modload = ioplib_getByName("modload\0");
    org_LoadStartModule  = ioplib_hookExportEntry(lib_modload,  7, Hook_LoadStartModule);
    org_StartModule      = ioplib_hookExportEntry(lib_modload,  8, Hook_StartModule);
    org_LoadModuleBuffer = ioplib_hookExportEntry(lib_modload, 10, Hook_LoadModuleBuffer);
    // check modload version
    if (lib_modload->version > 0x102) {
        org_ReferModuleStatus  = ioplib_hookExportEntry(lib_modload, 17, Hook_ReferModuleStatus);
        org_StopModule         = ioplib_hookExportEntry(lib_modload, 20, Hook_StopModule);
        org_UnloadModule       = ioplib_hookExportEntry(lib_modload, 21, Hook_UnloadModule);
        org_SearchModuleByName = ioplib_hookExportEntry(lib_modload, 22, Hook_SearchModuleByName);
    } else {
        struct FakeModule *modlist;
        // Change all REMOVABLE END values to RESIDENT END, if modload is old.
        for (modlist = fmd.fake; modlist->fname != NULL; modlist++) {
            if (modlist->returnStart == MODULE_REMOVABLE_END)
                modlist->returnStart = MODULE_RESIDENT_END;
        }
    }

    ioplib_relinkExports(lib_modload);

    return MODULE_RESIDENT_END;
}
