#include <libmain.hpp>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <vector>
#include <linux/limits.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <errno.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "jit/jit.hpp"
#include "log.hpp"

#include <sys/mman.h>

#include "../../../beatsaber-hook/shared/inline-hook/inlineHook.h"
#include "../../../beatsaber-hook/shared/utils/utils.h"

#include "../../../beatsaber-hook/shared/inline-hook/And64InlineHook.hpp"

using namespace modloader;


#define MOD_PATH_FMT "/sdcard/Android/data/%s/files/mods/"
#define MOD_TEMP_PATH_FMT "/data/data/%s/cache/curmod.so"

char *modPath;
char *modTempPath;

char *trimWhitespace(char *str)
{
  char *end;
  while(isspace((unsigned char)*str)) str++;
  if(*str == 0)
    return str;

  end = str + strlen(str) - 1;
  while(end > str && isspace((unsigned char)*end)) end--;

  end[1] = '\0';

  return str;
}

// MUST BE CALLED BEFORE LOADING MODS
const int setDataDirs()
{
    FILE *cmdline = fopen("/proc/self/cmdline", "r");
    if (cmdline) {
        //not sure what the actual max is, but path_max should cover it
        char application_id[PATH_MAX] = {0};
        fread(application_id, sizeof(application_id), 1, cmdline);
        fclose(cmdline);
        trimWhitespace(application_id);
        modTempPath = (char*)malloc(PATH_MAX);
        modPath = (char*)malloc(PATH_MAX);
        std::sprintf(modPath, MOD_PATH_FMT, application_id);
        std::sprintf(modTempPath, MOD_TEMP_PATH_FMT, application_id);
        return 0;
    } else {
        return -1;
    }    
}

int mkpath(char* file_path, mode_t mode) {
    for (char* p = strchr(file_path + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = '\0';
        if (mkdir(file_path, mode) == -1) {
            if (errno != EEXIST) {
                *p = '/';
                return -1;
            }
        }
        *p = '/';
    }
    return 0;
}

// TODO Find a way to avoid calling constructor on mods that have offsetless hooks in constructor
// Loads the mod at the given full_path
// Returns the dlopened handle
void* load_mod(const char* full_path) {
    // Calls the constructor on the mod by loading it
    log_print(INFO, "Loading mod: %s", full_path);
    int infile = open(full_path, O_RDONLY);
    off_t filesize = lseek(infile, 0, SEEK_END);
    lseek(infile, 0, SEEK_SET);
    unlink(modTempPath);
    int outfile = open(modTempPath, O_CREAT | O_WRONLY);
    sendfile(outfile, infile, 0, filesize);
    close(infile);
    close(outfile);
    chmod(modTempPath, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP);
    return dlopen(modTempPath, RTLD_NOW);
}

// Calls the init() function on the mod, if it exists
// This will be before il2cpp functionality is available
void init_mod(void* handle) {
    void (*init)(void);
    *(void**)(&init) = dlsym(handle, "init");
    if (init) {
        init();
    }
}

// Calls the load() function on the mod, if it exists
// This will be after il2cpp functionality is available
void load_mod(void* handle) {
    void (*load)(void);
    *(void**)(&load) = dlsym(handle, "load");
    if (load) {
        load();
    }
}

// Holds all constructed mods' dlopen handles
static std::vector<void*> modhandles = std::vector<void*>();
// Holds all constructed mods' full paths
static std::vector<char*> names = std::vector<char*>();
// Whether the mods have been constructed via construct_mods
static bool constructed = false;

void construct_mods() noexcept {
    log_print(INFO, "Constructing all mods!");

    struct dirent *dp;
    DIR *dir = opendir(modPath);

    while ((dp = readdir(dir)) != NULL)
    {
        if (strlen(dp->d_name) > 3 && !strcmp(dp->d_name + strlen(dp->d_name) - 3, ".so"))
        {
            char full_path[PATH_MAX];
            strcpy(full_path, modPath);
            strcat(full_path, dp->d_name);
            auto modHandle = load_mod(full_path);
            modhandles.push_back(modHandle);
            names.push_back(full_path);
        }
    }
    closedir(dir);
    constructed = true;
    log_print(INFO, "Done constructing mods!");
}

// Calls the init functions on all constructed mods
void init_mods() noexcept {
    if (!constructed) {
        log_print(ERROR, "Tried to initalize mods, but they are not yet constructed!");
        return;
    }
    log_print(INFO, "Initializing all mods!");

    auto n = names.begin();
    auto h = modhandles.begin();
    while (n != names.end() && h != modhandles.end()) {
        log_print(INFO, "Initializing mod: %s", *n);
        init_mod(*h);
        ++h;
        ++n;
    }

    log_print(INFO, "Initialized all mods!");
}

// Calls the load functions on all constructed mods
void load_mods() noexcept {
    if (!constructed) {
        log_print(ERROR, "Tried to load mods, but they are not yet constructed!");
        return;
    }
    log_print(INFO, "Loading all mods!");

    auto n = names.begin();
    auto h = modhandles.begin();
    while (n != names.end() && h != modhandles.end()) {
        log_print(INFO, "Loading mod: %s", *n);
        load_mod(*h);
        ++h;
        ++n;
    }

    log_print(INFO, "Loaded all mods!");
}

static void* imagehandle;
static void (*il2cppInit)(const char* domain_name);
// Loads the mods after il2cpp has been initialized
MAKE_HOOK_OFFSETLESS(il2cppInitHook, void, const char* domain_name)
{
    il2cppInitHook(domain_name);
    dlclose(imagehandle);
    load_mods();
}

extern "C" void modloader_preload() noexcept {
    logpf(ANDROID_LOG_VERBOSE, "modloader_preload called (should be really early)");

    log_print(INFO, "Welcome!");

    int modReady = 0;
    if (setDataDirs() != 0)
    {
         log_print(ERROR, "Unable to determine data directories.");
        modReady = -1;
    }
    else if (mkpath(modPath, 0) != 0)
    {
        log_print(ERROR, "Unable to access or create mod path at '%s'", modPath);
        modReady = -1;
    }
    else if (mkpath(modTempPath, 0) != 0)
    {
        log_print(ERROR, "Unable to access or create mod temporary path at '%s'", modTempPath);
        modReady = -1;
    }
    if (modReady != 0) {
        log_print(ERROR, "QuestHook failed to initialize, mods will not load.");
        return;
    }

    construct_mods();
}

extern "C" JNINativeInterface modloader_main(JavaVM* vm, JNIEnv* env, std::string_view loadSrc) noexcept {
    logpf(ANDROID_LOG_VERBOSE, "modloader_main called with vm: 0x%p, env: 0x%p, loadSrc: %s", vm, env, loadSrc.data());

    auto iface = jni::interface::make_passthrough_interface<JNINativeInterface>(&env->functions);

    init_mods();

    return iface;
}

extern "C" void modloader_accept_unity_handle(void* uhandle) noexcept {
    logpf(ANDROID_LOG_VERBOSE, "modloader_accept_unity_handle called with uhandle: 0x%p", uhandle);

    imagehandle = dlopen(IL2CPP_SO_PATH, RTLD_LOCAL | RTLD_LAZY);
    *(void**)(&il2cppInit) = dlsym(imagehandle, "il2cpp_init");
	log_print(INFO, "Loaded: il2cpp_init (%p)", il2cppInit);
    if (il2cppInit) {
        INSTALL_HOOK_DIRECT(il2cppInitHook, il2cppInit);
    } else {
        log_print(ERROR, "Failed to dlsym il2cpp_init!");
    }
}

CHECK_MODLOADER_PRELOAD;
CHECK_MODLOADER_MAIN;
CHECK_MODLOADER_ACCEPT_UNITY_HANDLE;