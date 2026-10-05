#include <pspkernel.h>
#include <pspmodulemgr.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <unistd.h>
#include "app_startup.h"
#include "shared.h"

extern int kuKernelLoadModule(const char *, int, void *);

char g_app_dir[512];
static char s_helper_path[512];
static SceUID s_owned_helper_uid = -1;
static int s_owned_helper_started;
static const char *s_error_step = "Media Engine helper";

const char *moonlight_startup_error_step(void) { return s_error_step; }
const char *moonlight_startup_helper_path(void) { return s_helper_path; }

static int helper_is_resident(void)
{
    SceUID ids[96];
    int count = 0, i;
    if (sceKernelGetModuleIdList(ids, sizeof(ids), &count) < 0) return 0;
    if (count > (int)(sizeof(ids) / sizeof(ids[0])))
        count = (int)(sizeof(ids) / sizeof(ids[0]));
    for (i = 0; i < count; ++i) {
        SceKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelQueryModuleInfo(ids[i], &info) == 0 &&
            strncmp(info.name, "MediaEngine", sizeof(info.name)) == 0)
            return 1;
    }
    return 0;
}

static int resolve_helper_path(int argc, char *argv[])
{
    static const char filename[] = "moonlight_me_helper.prx";
    const char *slash = NULL;
    size_t length;
    if (argc > 0 && argv && argv[0]) slash = strrchr(argv[0], '/');
    if (slash) {
        length = (size_t)(slash - argv[0]) + 1;
        if (length >= sizeof(g_app_dir)) return -1;
        memcpy(g_app_dir, argv[0], length);
        g_app_dir[length] = '\0';
    } else {
        if (!getcwd(g_app_dir, sizeof(g_app_dir))) return -1;
        length = strlen(g_app_dir);
        if (!length) return -1;
        if (g_app_dir[length - 1] != '/') {
            if (length + 1 >= sizeof(g_app_dir)) return -1;
            g_app_dir[length++] = '/';
            g_app_dir[length] = '\0';
        }
    }
    if (length + sizeof(filename) > sizeof(s_helper_path)) return -1;
    memcpy(s_helper_path, g_app_dir, length);
    memcpy(s_helper_path + length, filename, sizeof(filename));
    return 0;
}

int moonlight_startup_release_helper(void)
{
    int r, status = 0;
    if (s_owned_helper_uid < 0) return 0;
    if (s_owned_helper_started) {
        r = sceKernelStopModule(s_owned_helper_uid, 0, NULL, &status, NULL);
        if (r < 0 || status < 0) return r < 0 ? r : status;
        s_owned_helper_started = 0;
    }
    r = sceKernelUnloadModule(s_owned_helper_uid);
    if (r < 0) return r;
    s_owned_helper_uid = -1;
    return 0;
}

int moonlight_startup_load_helper(int argc, char *argv[])
{
    SceIoStat stat;
    int r, status = 0;
    if (s_owned_helper_uid >= 0 || helper_is_resident()) return 0;
    s_error_step = "Locate Media Engine helper";
    r = resolve_helper_path(argc, argv);
    if (r < 0) return r;
    s_error_step = "Read Media Engine helper";
    memset(&stat, 0, sizeof(stat));
    r = sceIoGetstat(s_helper_path, &stat);
    if (r < 0) return r;
    s_error_step = "Load Media Engine helper";
    r = kuKernelLoadModule(s_helper_path, 0, NULL);
    if (r < 0) return r;
    s_owned_helper_uid = r;
    s_error_step = "Start Media Engine helper";
    r = sceKernelStartModule(s_owned_helper_uid, 0, NULL, &status, NULL);
    if (r >= 0) s_owned_helper_started = 1;
    if (r < 0 || status < 0) {
        int error = r < 0 ? r : status;
        (void)moonlight_startup_release_helper();
        return error;
    }
    return 0;
}
