#include "utils.h"
#include <stdarg.h>
#include <installpkg.h>
#include <stdbool.h>
#include <orbis/AppInstUtil.h>
#include "defines.h"
#include <pthread.h> 
#include <sfo.hpp>
#include <iostream>
#include <ostream>
#include <fstream>
#include <sstream>
#include <curl/curl.h>

#if __has_include("<byteswap.h>")
#include <byteswap.h>
#else
// Replacement function for byteswap.h's bswap_32
uint32_t bswap_32(uint32_t val) {
  val = ((val << 8) & 0xFF00FF00 ) | ((val >> 8) & 0x00FF00FF );
  return (val << 16) | (val >> 16);
}
#endif

std::vector<uint8_t> readFile(std::string filename)
{
    // open the file:
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open())
    {
        log_info("Failed to open %s", filename.c_str());
        return std::vector<uint8_t>();
    }

    // Stop eating new lines in binary mode!!!
    file.unsetf(std::ios::skipws);

    // get its size:
    std::streampos fileSize;

    file.seekg(0, std::ios::end);
    fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // reserve capacity
    std::vector<uint8_t> vec;
    vec.reserve(fileSize);

    // read the data:
    vec.insert(vec.begin(),
               std::istream_iterator<uint8_t>(file),
               std::istream_iterator<uint8_t>());

    return vec;
}

extern std::vector<std::string> download_panel_text;
int PKG_ERROR(const char* name, int ret, dl_arg_t* args)
{
    if(!args)
        return ret;

    if(!args->is_threaded)
       msgok(WARNING,fmt::format("{}\nHEX: {}\nInt: {}\nFunction {}", getLangSTR(INSTALL_FAILED), ret, ret, name));

    log_error( "%s error: %x", name, ret);
    args->progress = 5.0;
    args->status = ret;

    return ret;
}

std::string normalize_version(const std::string &version) {
    std::istringstream iss(version);
    std::vector<int> parts;
    std::string part;
    while (std::getline(iss, part, '.')) {
        parts.push_back(std::stoi(part));
    }

    std::ostringstream oss;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            oss << '.';
        }
        oss << parts[i];
    }
    return oss.str();
}


bool GetInstalledVersion(std::string tid, std::string& version){
    
    std::string tmp = fmt::format("/system_data/priv/appmeta/{}/param.sfo", tid);
    if (!if_exists(tmp.c_str()))
         tmp = fmt::format("/system_data/priv/appmeta/external/{}/param.sfo", tid);

    //TODO make more effecient 
    std::vector<uint8_t> sfo_data = readFile(tmp);
    if(sfo_data.empty()){ 
       log_error("Could not read param.sfo");
       return false;
    }

    SfoReader sfo(sfo_data);
    version = sfo.GetValueFor<std::string>("VERSION");

    return true;
}
/* we use bgft heap menagement as init/fini as flatz already shown at 
 * https://github.com/flatz/ps4_remote_pkg_installer/blob/master/installer.c
 */

#define BGFT_HEAP_SIZE (1 * 1024 * 1024)

bool sceAppInst_done = false;
static bool   s_bgft_initialized = false;
static struct bgft_init_params  s_bgft_init_params;

void app_inst_util_fini(void) {
    if (!sceAppInst_done) {
        return;
    }

    sceAppInstUtilTerminate();
    sceAppInst_done = false;
}

bool bgft_init(void) {
    int ret;

    if (s_bgft_initialized) {
        goto done;
    }

    memset(&s_bgft_init_params, 0, sizeof(s_bgft_init_params));
    {
        s_bgft_init_params.heapSize = BGFT_HEAP_SIZE;
        s_bgft_init_params.heap = (uint8_t*)malloc(s_bgft_init_params.heapSize);
        if (!s_bgft_init_params.heap) {
            log_debug( "No memory for BGFT heap.");
            goto err;
        }
        memset(s_bgft_init_params.heap, 0, s_bgft_init_params.heapSize);
    }

    ret = sceBgftServiceIntInit(&s_bgft_init_params);
    if (ret) {
        log_debug( "sceBgftServiceIntInit failed: 0x%08X", ret);
        goto err_bgft_heap_free;
    }

    s_bgft_initialized = true;

done:
    return true;

err_bgft_heap_free:
    if (s_bgft_init_params.heap) {
        free(s_bgft_init_params.heap);
        s_bgft_init_params.heap = NULL;
    }

    memset(&s_bgft_init_params, 0, sizeof(s_bgft_init_params));

err:
    s_bgft_initialized = false;

    return false;
}

void bgft_fini(void) {
    int ret;

    if (!s_bgft_initialized) {
        return;
    }

    ret = sceBgftServiceIntTerm();
    if (ret) {
        log_debug( "sceBgftServiceIntTerm failed: 0x%08X", ret);
    }

    if (s_bgft_init_params.heap) {
        free(s_bgft_init_params.heap);
        s_bgft_init_params.heap = NULL;
    }

    memset(&s_bgft_init_params, 0, sizeof(s_bgft_init_params));

    s_bgft_initialized = false;
}

int sceAppInstUtilAppExists(const char* tid, int* flag);

bool app_inst_util_is_exists(const char* title_id, bool* exists) {
    int flag;

    if (!title_id) return false;

    if (!sceAppInst_done) {
        log_debug("Starting app_inst_util_init..");
        if (!app_inst_util_init()) {
            log_error("app_inst_util_init has failed...");
            return false;
        }
    }

    int ret = sceAppInstUtilAppExists(title_id, &flag);
    if (ret) {
        log_error("sceAppInstUtilAppExists failed: 0x%08X\n", ret);
        return false;
    }

    if (exists) *exists = flag;

    return true;
}

/* sample ends */
/* install package wrapper:
   init, (install), then clean AppInstUtil and BGFT
   for next install */
extern bool Download_icons;

bool pkg_is_patch(const char* src_dest) {

    // Read PKG header
    struct pkg_header hdr;
    static const uint8_t magic[] = { '\x7F', 'C', 'N', 'T' };
    // Open path
    int pkg = sceKernelOpen(src_dest, O_RDONLY, 0);
    if (pkg < 0) return false;

    sceKernelLseek(pkg, 0, SEEK_SET);
    sceKernelRead(pkg, &hdr, sizeof(struct pkg_header));

    sceKernelClose(pkg);

    if (memcmp(hdr.magic, magic, sizeof(magic)) != 0) {
        log_error("PKG Format is wrong");
        return false;
    }

    unsigned int flags = BE32(hdr.content_flags);

    if (flags & PKG_CONTENT_FLAGS_FIRST_PATCH || flags & PKG_CONTENT_FLAGS_SUBSEQUENT_PATCH || 
        flags & PKG_CONTENT_FLAGS_DELTA_PATCH || flags & PKG_CONTENT_FLAGS_CUMULATIVE_PATCH) 
    {
        return true;
    }

    return false;
}

/* Forward declaration — defined below */
void *install_prog(void* argument);

uint32_t pkginstall_remote(const char* pkg_url, dl_arg_t* ta, bool Auto_install)
{
    int  ret = -1;
    int  task_id = -1;
    char buffer[255];

    ta->status   = INSTALLING_APP;
    ta->progress = 0.0f;

    /*
     * All required metadata is already present in ta->token_d[], loaded from
     * the store DB by sql_index_tokens().  Use it directly — no extra HTTP
     * range-request to read the PKG header is needed.
     *
     *   ID         -> title_id (e.g. "CUSA00000")
     *   NAME       -> human-readable package name
     *   SIZE       -> package size (numeric bytes string in the DB, or falls
     *                 back to the HTTP content-length already fetched by
     *                 ini_dl_req)
     *   APPTYPE    -> "Base Game" / "Update" / "DLC" — used for patch routing
     *   CONTENT_ID -> full PS4 content ID (e.g.
     *                 "IV0002-CUSA00000_00-XXXXXXXXXXXXXXXX"), optional —
     *                 only present when the CDN's DB schema provides a
     *                 "content_id" column. Falls back to "" (matching prior
     *                 behavior) when absent.
     */
    const std::string& title_id   = ta->token_d[ID].off;
    const std::string& name       = ta->token_d[NAME].off;
    const std::string& size_str   = ta->token_d[SIZE].off;
    const std::string& apptype    = ta->token_d[APPTYPE].off;
    const std::string& content_id = ta->token_d[CONTENT_ID].off;

    if (title_id.empty()) {
        log_error("pkginstall_remote: title_id is empty — token_d not populated?");
        return PKG_ERROR("pkginstall_remote: empty title_id", ret, ta);
    }

    /* Derive package_size. Prefer the content-length from the HTTP HEAD
     * (already fetched by ini_dl_req via dl_from_url_v2) since it's an
     * authoritative raw byte count. The DB's "Size" column is a *human
     * formatted* string (e.g. "685.42 MB", see hb.js formatBytes()), NOT raw
     * bytes — strtoul() on it would silently parse only the leading digits
     * before the decimal point (e.g. 685), giving a wildly wrong byte count
     * that's still non-zero, so it must only be used as a last resort. */
    unsigned long pkg_size = 0;
    if (ta->contentLength.load() > 0)
        pkg_size = (unsigned long)ta->contentLength.load();

    if (pkg_size == 0 && !size_str.empty()) {
        char* end = nullptr;
        unsigned long parsed = strtoul(size_str.c_str(), &end, 10);
        if (end && end != size_str.c_str())
            pkg_size = parsed;
    }

    if (!app_inst_util_init())
        return PKG_ERROR("AppInstUtil", ret, ta);

    if (!bgft_init())
        return PKG_ERROR("BGFT_initialization", ret, ta);

    /* Foreground user — required by BGFT */
    int user_id = 0;
    ret = sceUserServiceGetForegroundUser(&user_id);
    if (ret) {
        log_error("sceUserServiceGetForegroundUser failed: 0x%08X", ret);
        return PKG_ERROR("sceUserServiceGetForegroundUser", ret, ta);
    }

    const std::string content_name = (!name.empty() ? name : title_id) + " via Store";
    snprintf(buffer, sizeof(buffer) - 1, "%s", content_name.c_str());
    log_info("%s", buffer);

    const std::string& picpath_str = ta->token_d[PICPATH].off;
    /* flatz's reference (both the remote-URL and StorageEx paths) always
     * falls back to an empty string here, never a made-up local path — BGFT
     * appears to validate/open a non-empty icon_path, so pointing it at a
     * file that doesn't actually exist on the PS4 (this repo has no
     * fakepic.png asset anywhere) is a plausible cause of an immediate
     * registration failure (e.g. SCE_BGFT_ERROR_INVALID_PARAMETER). */
    const char* icon_path = (!picpath_str.empty() && if_exists(picpath_str.c_str()))
        ? picpath_str.c_str()
        : "";

    /* "Update" apptype means this is a patch PKG */
    const bool is_patch = (apptype == "Update");

    /* package_type is NOT a generic "PS4" string - BGFT validates it against
     * specific per-content-type values. Confirmed against njzydark/PS4RPI's
     * server.c (a real, working remote installer), which maps the PKG's
     * actual content type to "PS4GD" (base game), "PS4AC" (DLC/additional
     * content), "PS4AL", or "PS4DP" (patch) - never a bare "PS4". Passing an
     * unrecognized value here is a very plausible cause of BGFT
     * unconditionally rejecting registration with
     * SCE_BGFT_ERROR_INVALID_PARAMETER regardless of how valid every other
     * field is, since we don't have the raw PKG content_type enum available
     * for remote (no local file) installs, derive the equivalent from the
     * apptype string already resolved from the DB's APPTYPE/CATEGORY. */
    const char* package_type = "PS4GD";
    if (apptype == "DLC")
        package_type = "PS4AC";
    else if (apptype == "Update")
        package_type = "PS4DP";

    struct bgft_download_param_ex download_params;
    memset(&download_params, 0, sizeof(download_params));
    download_params.param.user_id            = user_id;
    download_params.param.entitlement_type   = 5;
    download_params.param.id                 = !content_id.empty() ? content_id.c_str() : "";
    download_params.param.content_url        = pkg_url;
    download_params.param.content_ex_url     = "";
    download_params.param.content_name       = buffer;
    download_params.param.icon_path          = icon_path;
    download_params.param.sku_id             = "";
    download_params.param.playgo_scenario_id = "0";
    download_params.param.option             = BGFT_TASK_OPTION_DISABLE_CDN_QUERY_PARAM;
    download_params.param.release_date       = "";
    download_params.param.package_type       = package_type;
    download_params.param.package_sub_type   = "";
    download_params.param.package_size       = pkg_size;
    download_params.slot                     = 0;

    /* Log every field handed to BGFT so a registration failure (e.g. the
     * kernel rejecting an empty/malformed content_id with
     * SCE_BGFT_ERROR_INVALID_PARAMETER = 0x80990004) can be root-caused
     * from store.log alone, without needing to reproduce interactively. */
    log_info("pkginstall_remote params: user_id=%d entitlement_type=%d id='%s' url='%s' "
             "name='%s' icon='%s' sku_id='%s' playgo='%s' option=0x%x release_date='%s' "
             "package_type='%s' package_sub_type='%s' size=%u is_patch=%d",
             user_id, download_params.param.entitlement_type, download_params.param.id,
             pkg_url, buffer, icon_path, download_params.param.sku_id,
             download_params.param.playgo_scenario_id, (unsigned int)download_params.param.option,
             download_params.param.release_date, download_params.param.package_type,
             download_params.param.package_sub_type, download_params.param.package_size,
             (int)is_patch);

    {
        int retry = 0;
        const int MAX_RETRIES = 2;
        while (true) {
            log_info("%s: registering task (is_patch=%d)", __FUNCTION__, (int)is_patch);
            if (!is_patch) {
                /* Call the toolchain's own statically-linked stub directly
                 * (matches njzydark/PS4RPI's proven-working fix for this
                 * exact SCE_BGFT_ERROR_INVALID_PARAMETER / 0x80990004 -
                 * their bug was manually re-resolving BGFT functions by a
                 * mix of "Int"/no-"Int" names via sceKernelDlsym(), and the
                 * fix was to stop doing that and just use the toolchain's
                 * correctly-named import). The real fix for our case was
                 * bgft_init()/bgft_fini() calling the wrong (no "Int")
                 * sceBgftServiceInit/sceBgftServiceTerm names, leaving BGFT
                 * never properly initialized in the first place. */
                ret = sceBgftServiceIntDownloadRegisterTask(&download_params.param, &task_id);
            } else {
                ret = sceBgftServiceIntDebugDownloadRegisterPkg(&download_params.param, &task_id);
            }
            if (ret == SCE_BGFT_ERROR_ALREADY_REGISTERED || ret == SCE_BGFT_ERROR_ALREADY_INSTALLED) {
                if (++retry > MAX_RETRIES)
                    return PKG_ERROR("sceBgftRegisterTask (retry limit)", ret, ta);
                ret = sceAppInstUtilAppUnInstall(title_id.c_str());
                if (ret != 0)
                    return PKG_ERROR("sceAppInstUtilAppUnInstall", ret, ta);
                continue;
            }
            else if (ret)
                return PKG_ERROR("sceBgftRegisterTask", ret, ta);
            break;
        }
    }

    log_info("Task ID(s): 0x%08X", task_id);

    struct install_args* args = new install_args;
    args->title_id  = title_id;
    args->task_id   = task_id;
    args->l         = ta;
    args->path      = ""; /* no local file */
    args->is_thread = !Auto_install;
    args->delete_pkg = false; /* nothing to delete on disk */

    if (Auto_install) {
        ret = sceBgftServiceDownloadStartTask(task_id);
        if (ret) { delete args; return PKG_ERROR("sceBgftDownloadStartTask", ret, ta); }
        install_prog((void*)args); /* install_prog deletes args */
    }
    else if (set.Legacy_Install.load()) {
        ret = sceBgftServiceDownloadStartTask(task_id);
        if (ret) { delete args; return PKG_ERROR("sceBgftDownloadStartTask", ret, ta); }
        pthread_t thread = 0;
        ret = pthread_create(&thread, NULL, install_prog, (void*)args); /* install_prog deletes args */
        log_debug("pthread_create for %x, ret:%d", task_id, ret);
        if (ret == 0)
            pthread_detach(thread);
        else {
            delete args;
            /* No thread will monitor this task — stop it rather than
             * leaving it running in the background with no progress
             * tracking. */
            sceBgftServiceDownloadStopTask(task_id);
            return PKG_ERROR("pthread_create", ret, ta);
        }
    }
    else {
        ret = sceBgftServiceDownloadStartTask(task_id);
        if (ret) {
            delete args;
            return PKG_ERROR("sceBgftServiceDownloadStartTask", ret, ta);
        } else {
            if (icon_panel && !icon_panel->item_d[ta->g_idx].token_d[ID].off.empty()) {
                icon_panel->item_d[ta->g_idx].interruptible = false;
                icon_panel->item_d[ta->g_idx].update_status = NO_UPDATE;
                download_panel->item_d[0].token_d[0].off = download_panel_text[0] = getLangSTR(REINSTALL_APP);
            }
            ta->g_idx  = -1;
            ta->status = READY;
            layout_refresh_VBOs();
            log_info("package successfully started in the background");
            delete args;
        }
    }

    log_info("%s(%s) done.", __FUNCTION__, pkg_url);
    ta->dst.clear();

    return 0;
}

void *install_prog(void* argument)
{
    struct install_args* args = (install_args*) argument;
    SceBgftTaskProgress progress_info;
    bool is_threaded = args->is_thread;

    log_info("Starting PKG Install");
    args->l->progress = 0.;
    args->l->status = INSTALLING_APP;
    // trigger refresh of Queue active count
    left_panel2->vbo_s = ASK_REFRESH;

    while (args->l->progress < 99)
    {
        memset(&progress_info, 0, sizeof(progress_info));

        int ret = sceBgftServiceDownloadGetProgress(args->task_id, &progress_info);
        if (ret) {
          // PKG_ERROR("sceBgftDownloadGetProgress", ret);
           args->l->progress = 0.;
           args->l->status = ret;
           goto clean;
        }

        if (progress_info.transferred > 0 && progress_info.error_result != 0) {
             args->l->progress = 0.;
             args->l->status = progress_info.error_result;
             goto clean;
        }

        args->l->progress = (uint32_t)(((float)progress_info.transferred / progress_info.length) * 100.f);


        if (progress_info.transferred % (4096 * 128) == 0)
             log_debug("%s, Install_Thread: reading data, %lub / %lub (%%%lf) ERR: %i", __FUNCTION__, progress_info.transferred, progress_info.length,  args->l->progress.load(), progress_info.error_result);

    }

    if (progress_info.error_result != 0) {
            args->l->progress = 0.;
            args->l->status = progress_info.error_result;
            log_error("Installation of %s has failed with code 0x%x", args->title_id.c_str(), progress_info.error_result);
    }
    else{
       args->l->status = COMPLETED;
    }
    
clean:
    log_info("Finalizing Memory...");
    log_info("Deleting PKG %s...", args->path.c_str());
    icon_panel->mtx.lock();
    download_panel->mtx.lock();
    if(!games.empty()){ // reset gaame update status to latest status
        games[args->l->g_idx].interruptible = false;
        if(games[args->l->g_idx].update_status.load() == UPDATE_FOUND){
         if(updates_counter.load() > 0){
            updates_counter--;
         }

        games[args->l->g_idx].update_status = NO_UPDATE;
       }
       download_panel->item_d[0].token_d[0].off =  download_panel_text[0] = getLangSTR(REINSTALL_APP);
    }
    download_panel->mtx.unlock();
    icon_panel->mtx.unlock();
    args->l->g_idx = -1;
   
    delete args;

    // trigger refresh of Queue active count
    left_panel2->vbo_s = ASK_REFRESH;
    log_info("Set Status: Ready");

    if(is_threaded)
       pthread_exit(NULL);

    return NULL;
}
bool app_inst_util_init(void) {
    int ret;

    if (sceAppInst_done) {
        goto done;
    }

    ret = sceAppInstUtilInitialize();
    if (ret) {
        log_debug( "sceAppInstUtilInitialize failed: 0x%08X", ret);
        goto err;
    }

    sceAppInst_done = true;

done:
    return true;

err:
    sceAppInst_done = false;

    return false;
}


uint32_t pkginstall(const char *fullpath, dl_arg_t* ta, bool Auto_install)
{
    char title_id[16];
    int  is_app, ret = -1;
    int  task_id = -1;
    char buffer[255];

    ta->status = INSTALLING_APP;
    ta->progress = 0.0f;

    if( if_exists(fullpath) )
    {
      if (sceAppInst_done) {
          log_info("Initializing AppInstUtil...");

          if (!app_inst_util_init())
              return PKG_ERROR("AppInstUtil", ret, ta);
      }
        
        log_info("Initializing BGFT...");
        if (!bgft_init()) {
            return PKG_ERROR("BGFT_initialization", ret, ta);
        }

        ret = sceAppInstUtilGetTitleIdFromPkg(fullpath, title_id, &is_app);
        if (ret) 
            return PKG_ERROR("sceAppInstUtilGetTitleIdFromPkg", ret, ta);

        /* Foreground user — required by BGFT */
        int user_id = 0;
        ret = sceUserServiceGetForegroundUser(&user_id);
        if (ret) {
            log_error("sceUserServiceGetForegroundUser failed: 0x%08X", ret);
            return PKG_ERROR("sceUserServiceGetForegroundUser", ret, ta);
        }

        snprintf(buffer, 254, "%s via Store", title_id);
        log_info( "%s", buffer);

        const std::string& picpath_str = ta->token_d[PICPATH].off;
        const char* icon_path = (!picpath_str.empty() && if_exists(picpath_str.c_str()))
            ? picpath_str.c_str()
            : "";

        /* Detect patch by reading the local PKG header */
        const bool is_patch = pkg_is_patch(fullpath);

        /* package_type must match a real BGFT-recognized value ("PS4GD" /
         * "PS4AC" / "PS4AL" / "PS4DP"), not a generic "PS4" - see the same
         * fix/comment in pkginstall_remote() above. Use the DB's APPTYPE
         * token (already available on ta) the same way. */
        const std::string& apptype = ta->token_d[APPTYPE].off;
        const char* package_type = "PS4GD";
        if (apptype == "DLC")
            package_type = "PS4AC";
        else if (apptype == "Update" || is_patch)
            package_type = "PS4DP";

        struct bgft_download_param_ex download_params;
        memset(&download_params, 0, sizeof(download_params));
        download_params.param.user_id            = user_id;
        download_params.param.entitlement_type   = 5;
        download_params.param.id                 = "";
        download_params.param.content_url        = fullpath;
        download_params.param.content_name       = buffer;
        download_params.param.icon_path          = icon_path;
        download_params.param.playgo_scenario_id = "0";
        download_params.param.option             = BGFT_TASK_OPTION_INVISIBLE;
        download_params.param.package_type       = package_type;
        download_params.param.package_sub_type   = "";
        download_params.slot                     = 0;

    retry:
        log_info("%s: registering task (is_patch=%d)", __FUNCTION__, (int)is_patch);
        if (!is_patch) {
            ret = sceBgftServiceIntDownloadRegisterTaskByStorageEx(&download_params, &task_id);
        } else {
            ret = sceBgftServiceIntDebugDownloadRegisterPkg(&download_params.param, &task_id);
        }
        if(ret == SCE_BGFT_ERROR_ALREADY_REGISTERED || ret == SCE_BGFT_ERROR_ALREADY_INSTALLED)
        {
            ret = sceAppInstUtilAppUnInstall(&title_id[0]);
            if(ret != 0)
               return PKG_ERROR("sceAppInstUtilAppUnInstall", ret, ta);

            goto retry;

        }
        else if(ret) 
            return PKG_ERROR("sceBgftRegisterTask", ret, ta);
        

        log_info("Task ID(s): 0x%08X", task_id);

        ret = sceBgftServiceDownloadStartTask(task_id);
        if(ret) 
            return PKG_ERROR("sceBgftDownloadStartTask", ret, ta);
    }
    else//bgft_download_get_task_progress
        return PKG_ERROR("no file at", ret, ta);


    struct install_args* args = new install_args;
    args->title_id = title_id;
    args->task_id = task_id;
    args->l = ta;
    args->path = fullpath;
    args->is_thread = !Auto_install;
    args->delete_pkg = true; //STORE DOWNLOADS ONLY

     //Both Auto install and Show install progress will set status to INSTALL_APP
    if (Auto_install){ //Auto Install (is being called by the download thread)
        install_prog((void*)args);
    }
    else if (set.Legacy_Install.load()){ //is "Show Install Progess" enabled, if so make a thread
        pthread_t thread = 0;
        ret = pthread_create(&thread, NULL, install_prog, (void*)args);
        log_debug("pthread_create for %x, ret:%d", task_id, ret);
    }
    else { // Default, Auto install and Show install progress are disabled
          //  so we let the PS4 INSTALL it in the Background, 
        ret = sceBgftServiceDownloadStartTask(args->task_id);
        if (ret) {
            return PKG_ERROR("sceBgftServiceDownloadStartTask", ret, ta);
        }
        else { // too bad we cant delete the file with this option
            if(icon_panel && !icon_panel->item_d[args->l->g_idx].token_d[ID].off.empty()){
               icon_panel->item_d[ta->g_idx].interruptible = false;
               icon_panel->item_d[ta->g_idx].update_status = NO_UPDATE;
               
               download_panel->item_d[0].token_d[0].off =  download_panel_text[0] = getLangSTR(REINSTALL_APP);
            }
            ta->g_idx = -1;
            ta->status = READY;
            layout_refresh_VBOs();

            log_info("package successfully started in the background");
        }

    }

    log_info( "%s(%s) done.", __FUNCTION__, fullpath);
    ta->dst.clear();

    return 0;
}