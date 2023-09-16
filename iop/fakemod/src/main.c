#include <loadcore.h>
#include <stdio.h>
#include <sysclib.h>
#include <xmodload.h>

#include "fakemod.h"
#include "ioplib.h"

#ifdef DEBUG
#define DPRINTF(args...) printf(args)
#else
#define DPRINTF(args...)
#endif

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

#ifdef DEBUG
//--------------------------------------------------------------
static void print_args(int arg_len, char *args)
{
    // Multiple null terminated strings together

    // Print first string
    if (arg_len > 0)
        DPRINTF("- args[]=%s\n", args);

    // Search for more strings
    while(arg_len>1) {
        if (*args == 0) {
            DPRINTF("- args[]=%s\n", &args[1]);
        }
        arg_len--;
        args++;
    }
}
#endif

//--------------------------------------------------------------
static int Hook_LoadStartModule(char *modpath, int arg_len, char *args, int *modres)
{
    struct FakeModule *mod;

    DPRINTF("%s(%s)\n", __FUNCTION__, modpath);
#ifdef DEBUG
    print_args(arg_len, args);
#endif

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

        DPRINTF("- FAKING! id=0x%x, rv=%d, modres=%d\n", mod->id, rv, *modres);
        return rv;
    }

    return org_LoadStartModule(modpath, arg_len, args, modres);
}

//--------------------------------------------------------------
static int Hook_StartModule(int id, char *modname, int arg_len, char *args, int *modres)
{
    struct FakeModule *mod;

    DPRINTF("%s(0x%x, %s)\n", __FUNCTION__, id, modname);
#ifdef DEBUG
    print_args(arg_len, args);
#endif

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

        DPRINTF("- FAKING! id=0x%x, rv=%d, modres=%d\n", mod->id, rv, *modres);
        return rv;
    }

    return org_StartModule(id, modname, arg_len, args, modres);
}

//--------------------------------------------------------------
static int Hook_LoadModuleBuffer(void *ptr)
{
    struct FakeModule *mod;
    const char *modname = (const char *)ptr + 0x8e;

    DPRINTF("%s() modname = '%s'\n", __FUNCTION__, modname);

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

        DPRINTF("- FAKING! id=0x%x, rv=%d\n", mod->id, rv);
        return rv;
    }

    return org_LoadModuleBuffer(ptr);
}

//--------------------------------------------------------------
static int Hook_StopModule(int id, int arg_len, char *args, int *modres)
{
    struct FakeModule *mod;

    DPRINTF("%s(0x%x)\n", __FUNCTION__, id);
#ifdef DEBUG
    print_args(arg_len, args);
#endif

    mod = checkFakemodById(id, fmd.fake);
    if (mod != NULL) {
        DPRINTF("- FAKING! id=0x%x\n", mod->id);

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

    DPRINTF("%s(0x%x)\n", __FUNCTION__, id);

    mod = checkFakemodById(id, fmd.fake);
    if (mod != NULL) {
        DPRINTF("- FAKING! id=0x%x\n", mod->id);

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

    DPRINTF("%s(%s)\n", __FUNCTION__, modname);

    mod = checkFakemodByName(modname, fmd.fake);
    if (mod != NULL) {
        int rv = (mod->returnLoad == 0) ? mod->id : -202 /*KE_UNKNOWN_MODULE*/;
        DPRINTF("- FAKING! id=0x%x rv=%d\n", mod->id, rv);
        return rv;
    }

    return org_SearchModuleByName(modname);
}

//--------------------------------------------------------------
static int Hook_ReferModuleStatus(int id, ModuleStatus *status)
{
    struct FakeModule *mod;

    DPRINTF("%s(0x%x)\n", __FUNCTION__, id);

    mod = checkFakemodById(id, fmd.fake);
    if (mod != NULL && (mod->prop & FAKE_PROP_REPLACE) == 0 && mod->returnLoad == 0) {
        DPRINTF("- FAKING! id=0x%x\n", mod->id);
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

    // Change string index to string pointers
    DPRINTF("Fake module list:\n");
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
            DPRINTF("  %d: %12s | %-14s | 0x%3x | %3d | %d | 0x%x\n", i, fm->fname, fm->name, fm->version, fm->returnLoad, fm->returnStart, fm->prop);
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
