#include "base.hpp"
#include "logging.hpp"
#include "mountinfo.hpp"
#include "utils.hpp"

using namespace std;

#define mount(a,b,c,d,e) verbose_mount(a,b,c,d,e)
#define umount2(a,b) verbose_umount(a,b)

#define UNDER(s) (starts_with(info.target.data(), s "/") || info.target == s)

#define MAKEDIR(s) \
    if (std::find(mountpoint.begin(), mountpoint.end(), "/" s) != mountpoint.end()) { \
        mkdir(std::string(tmp_dir + "/" s).data(), 0755); \
    }

#define CLEANUP \
    LOGI("clean up\n"); \
    umount2(tmp_dir.data(), MNT_DETACH); \
    rmdir(tmp_dir.data());

int log_fd = -1;
std::string tmp_dir;

int main(int argc, const char **argv) {
    bool overlay = false;
    FILE *fp = fopen("/proc/filesystems", "re");
    if (fp) {
        char buf[1024];
        while (fgets(buf, sizeof(buf), fp)) {
            buf[strlen(buf)-1] = '\0';
            if (strcmp(buf + 6, "overlay") == 0) {
                overlay = true;
                break;
            }
        }
        fclose(fp);
    }
    if (!overlay) {
        printf("No overlay supported by kernel!\n");
        return 1;
    }
    if (argc<2) {
        printf("You forgot to tell me the write-able folder :v\n");
        return 1;
    }
    if (strcmp(argv[1], "--test") == 0) {
        argc--;
        argv++;
        if (argc >= 3 && strcmp(argv[1], "--check-ext4") == 0) {
            struct statfs stfs{};
            return (statfs(argv[2], &stfs) == 0 && stfs.f_type == EXT4_SUPER_MAGIC)?
                0 : 1;
        }
        return 0;
    } else if (argv[1][0] != '/') {
        printf("Please tell me the full path of folder >:)\n");
        return 1;
    }

    const char *OVERLAY_MODE_env = getenv("OVERLAY_MODE");
    int OVERLAY_MODE = (OVERLAY_MODE_env)? atoi(OVERLAY_MODE_env) : 0;

    struct stat z;
    if (stat(argv[1], &z)) {
        printf("%s does not exist!\n", argv[1]);
        return 1;
    }
    if (!S_ISDIR(z.st_mode)) {
        printf("This is not folder %s\n", argv[1]);
        return 1;
    }
    const char *mirrors =
    (argc >= 3 && argv[2][0] == '/' && stat(argv[2], &z) == 0 && S_ISDIR(z.st_mode))?
        argv[2] : nullptr;
    std::vector<string> mountpoint;
    std::vector<mount_info> mountinfo;

    log_fd = open("/cache/overlayfs.log", O_RDWR | O_CREAT | O_APPEND, 0666);

    tmp_dir = std::string("/mnt/") + "overlayfs_" + random_strc(20);
    if (mkdir(tmp_dir.data(), 750) != 0) {
        LOGE("Cannot create temp folder, please make sure /mnt is clean and write-able!\n");
        return -1;
    }
    mkdir(std::string(std::string(argv[1]) + "/upper").data(), 0750);
    mkdir(std::string(std::string(argv[1]) + "/worker").data(), 0750);
    mount("tmpfs", tmp_dir.data(), "tmpfs", 0, nullptr);

    // trim mountinfo
    do {
        auto current_mount_info = parse_mount_info("self");
        std::reverse(current_mount_info.begin(), current_mount_info.end());
        for (auto &info : current_mount_info) {
            struct stat st;
            // skip mount under another mounr
            if (stat(info.target.data(), &st) || info.device != st.st_dev)
                continue;
            if (UNDER("/system") ||
                 UNDER("/vendor") ||
                 UNDER("/system_ext") ||
                 UNDER("/product")) {
                for (auto &s : mountpoint) {
                    if (s == info.target)
                        goto next_mountpoint;
                 }
                //printf("new mount: %s\n", info.target.data());
                mountpoint.emplace_back(info.target);
                mountinfo.emplace_back(info);
            }
            next_mountpoint:
            continue;
        }
    } while(false);

    struct mount_info system;
    system.target = "/system";
    system.type = "ext4";
    if (std::find(mountpoint.begin(), mountpoint.end(), "/system") == mountpoint.end()) {
        mountinfo.emplace_back(system);
        mountpoint.emplace_back("/system");
    }

    MAKEDIR("system")
    MAKEDIR("vendor")
    MAKEDIR("system_ext")
    MAKEDIR("product")

    mountpoint.clear();

    LOGI("** Prepare mounts\n");
    // mount overlayfs for root of /system /vendor /product /system_ext and restore stock mounts if possible
    std::reverse(mountinfo.begin(), mountinfo.end());
    for (auto &mnt : mountinfo) {
        auto info = mnt.target;
        std::string tmp_mount = tmp_dir + info;
        struct stat st;
        {
            char *con;
            std::string upperdir = std::string(argv[1]) + "/upper" + info;
            std::string workerdir = std::string(argv[1]) + "/worker" + info;
            std::string masterdir = std::string(argv[1]) + "/master" + info;
            if (stat(info.data(), &st) == 0 && !S_ISDIR(st.st_mode))
                goto bind_mount;
            {
                char *s = strdup(info.data());
                char *ss = s;
                while ((ss = strchr(ss, '/')) != nullptr) {
                    ss[0] = '\0';
                    auto sub = std::string(argv[1]) + "/upper" + s;
                    if (mkdir(sub.data(), 0755) == 0 && getfilecon(s, &con) >= 0) {
                        LOGD("clone attr [%s] from [%s]\n", con, s);
                        chown(sub.data(), getuidof(s), getgidof(s));
                        chmod(sub.data(), getmod(s));
                        setfilecon(sub.data(), con);
                        freecon(con);
                    }
                    ss[0] = '/';
                    ss++;
                }
                free(s);
            };
            {
                if (mkdir(upperdir.data(), 0755) == 0 && getfilecon(info.data(), &con) >= 0) {
                    LOGD("clone attr [%s] from [%s]\n", con, info.data());
                    chown(upperdir.data(), getuidof(info.data()), getgidof(info.data()));
                    chmod(upperdir.data(), getmod(info.data()));
                    setfilecon(upperdir.data(), con);
                    freecon(con);
                }
                mkdirs(workerdir.data(), 0755);
    
                if (!is_dir(upperdir.data()) ||
                    !is_dir(workerdir.data())) {
                    LOGD("setup upperdir or workdir failed!\n");
                    CLEANUP
                    return 1;
                }
            }
            {
                std::string opts;
                opts += "lowerdir=";
                if (stat(masterdir.data(), &st) == 0 && S_ISDIR(st.st_mode))
                    opts += masterdir + ":";
                opts += info.data();
                opts += ",upperdir=";
                opts += upperdir;
                opts += ",workdir=";
                opts += workerdir;
                
                // 0 - read-only
                // 1 - read-write default
                // 2 - read-only locked
                
                if (OVERLAY_MODE == 2 || mount("overlay", tmp_mount.data(), "overlay", ((OVERLAY_MODE == 1)? 0 : MS_RDONLY), opts.data())) {
                    opts = "lowerdir=";
                    if (stat(masterdir.data(), &st) == 0 && S_ISDIR(st.st_mode))
                        opts += masterdir + ":";
                    opts += upperdir;
                    opts += ":";
                    opts += info.data();
                    if (mount("overlay", tmp_mount.data(), "overlay", 0, opts.data())) {
                        LOGW("mount overlayfs failed, fall to bind mount!\n");
                        goto bind_mount;
                    }
                }
            }
            goto mount_done;
               
            bind_mount:
            if (mount(info.data(), tmp_mount.data(), nullptr, MS_BIND, nullptr)) {
                // mount fails
                LOGE("mount failed, abort!\n");
                CLEANUP
                return 1;
            }
            
            mount_done:
            mountpoint.emplace_back(info);
        }
    }


    LOGI("** Loading overlayfs\n");
    std::vector<string> mounted;
    for (auto &info : mountpoint) {
        std::string tmp_mount = tmp_dir + info;
        if (mount(tmp_mount.data(), info.data(), nullptr, MS_BIND, nullptr) ||
#undef mount
            mount("", info.data(), nullptr, MS_PRIVATE, nullptr) ||
            mount("", info.data(), nullptr, MS_SHARED, nullptr)) {
            LOGE("mount failed, abort!\n");
            // revert all mounts
            std::reverse(mounted.begin(), mounted.end());
            for (auto &dir : mounted) {
                umount2(dir.data(), MNT_DETACH);
            }
            CLEANUP
            return 1;
        }
        mounted.emplace_back(info);
    }
    // mount to magisk mirrors
    if (mirrors != nullptr) {
        for (auto &info : mountpoint) {
            std::string tmp_mount = tmp_dir + info;
            std::string mirror_dir = string(mirrors) + info;
            mount(tmp_mount.data(), mirror_dir.data(), nullptr, MS_BIND, nullptr);
            mount("", mirror_dir.data(), nullptr, MS_PRIVATE, nullptr);
            mount("", mirror_dir.data(), nullptr, MS_SHARED, nullptr);
        }
    }
    LOGI("mount done!\n");
    CLEANUP
    return 0;
}
