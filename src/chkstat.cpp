/* Copyright (c) 2004 SuSE Linux AG
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING); if not, write to the
 * Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 *
 ****************************************************************
 */

#ifndef _GNU_SOURCE
#   define _GNU_SOURCE
#endif
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/capability.h>
#include <fcntl.h>
#include <sys/param.h>
#include <limits.h>

// local headers
#include "chkstat.h"

// C++
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <utility>

#define BAD_LINE() \
    fprintf(stderr, "bad permissions line %s:%d\n", permfiles[i], lcnt)

struct perm
{
    struct perm *next;
    char *file;
    char *owner;
    char *group;
    mode_t mode;
    cap_t caps;
};

struct perm *permlist;
char **checklist;
size_t nchecklist;
uid_t euid;
const char *root;
size_t rootl;
size_t nlevel;
const char** level;
int default_set = 1;
char** permfiles = NULL;
size_t npermfiles = 0;
std::string config_root;

struct perm*
add_permlist(char *file, const char *p_owner, const char *p_group, mode_t mode)
{
    struct perm *ec, **epp;

    char *owner = strdup(p_owner);
    char *group = strdup(p_group);
    if (rootl)
    {
        char *nfile;
        nfile = (char*)malloc(strlen(file) + rootl + (*file != '/' ? 2 : 1));
        if (nfile)
        {
            strcpy(nfile, root);
            if (*file != '/')
                strcat(nfile, "/");
            strcat(nfile, file);
        }
        file = nfile;
    }
    else
    {
        file = strdup(file);
    }

    if (!owner || !group || !file)
    {
        perror("permlist entry alloc");
        exit(1);
    }

    for (epp = &permlist; (ec = *epp) != 0; )
    {
        if (!strcmp(ec->file, file))
        {
            *epp = ec->next;
            free(ec->file);
            free(ec->owner);
            free(ec->group);
            free(ec);
        }
        else
            epp = &ec->next;
    }

    ec = (perm*)malloc(sizeof(struct perm));
    if (ec == 0)
    {
        perror("permlist entry alloc");
        exit(1);
    }
    ec->file = file;
    ec->owner = owner;
    ec->group = group;
    ec->mode = mode;
    ec->caps = NULL;
    ec->next = 0;
    *epp = ec;
    return ec;
}

int
in_checklist(const char *e)
{
    size_t i;
    for (i = 0; i < nchecklist; i++)
    {
        if (!strcmp(e, checklist[i]))
        {
            return 1;
        }
    }
    return 0;
}

void
add_checklist(const char *e)
{
    if (in_checklist(e))
        return;

    char *copy = strdup(e);
    if (copy == 0)
    {
        perror("checklist entry alloc");
        exit(1);
    }
    if ((nchecklist & 63) == 0)
    {
        checklist = (char**)realloc(checklist, sizeof(char *) * (nchecklist + 64));
        if (checklist == 0)
        {
            perror("checklist alloc");
            exit(1);
        }
    }
    checklist[nchecklist++] = copy;
}

int
readline(FILE *fp, char *buf, size_t len)
{
    size_t l;
    if (!fgets(buf, (int)len, fp))
        return 0;
    l = strlen(buf);
    if (l && buf[l - 1] == '\n')
    {
        l--;
        buf[l] = 0;
    }
    if (l + 1 < len)
        return 1;
    fprintf(stderr, "warning: buffer overrun in line starting with '%s'\n", buf);
    int c;
    while ((c = getc(fp)) != EOF && c != '\n')
        ;
    buf[0] = 0;
    return 1;
}

int
in_level(const char *e)
{
    size_t i;
    for (i = 0; i < nlevel; i++)
    {
        if (!strcmp(e, level[i]))
            return 1;

    }
    return 0;
}

void
ensure_array(void** array, size_t* size)
{
    if ((*size & 63) == 0)
    {
        *array = realloc(*array, sizeof(char *) * (*size + 64));
        if (*array == NULL)
        {
            perror("array alloc");
            exit(1);
        }
    }
}

void
add_level(const char *e)
{
    if (in_level(e))
        return;
    e = strdup(e);
    if (e == 0)
    {
        perror("level entry alloc");
        exit(1);
    }
    ensure_array((void**)&level, &nlevel);
    level[nlevel++] = e;
}

static inline int isquote(char c)
{
    return (c == '"' || c == '\'');
}

int
parse_sysconf(const char* file, bool parse_levels, bool *fscaps_configured)
{
    FILE* fp;
    char line[PATH_MAX];
    char* p;

    if ((fp = fopen(file, "r")) == 0)
    {
        fprintf(stderr, "error opening: %s: %s\n", file, strerror(errno));
        return 0;
    }

    while (readline(fp, line, sizeof(line)))
    {
        if (!*line)
            continue;

        for (p = line; *p == ' '; ++p);

        if (!*p || *p == '#')
            continue;

        if (!strncmp(p, "PERMISSION_SECURITY=", 20))
        {
            if (!parse_levels)
                continue;

            p+=20;
            if (isquote(*p))
                ++p;
            p = strtok(p, " ");
            if (p && !isquote(*p))
            {
                do
                {
                    if (isquote(p[strlen(p)-1]))
                    {
                        p[strlen(p)-1] = '\0';
                    }
                    if (*p && strcmp(p, "local"))
                        add_level(p);
                }
                while ((p = strtok(NULL, " ")));
            }
        }
        else if (!strncmp(p, "CHECK_PERMISSIONS=", 18))
        {
            p+=18;
            if (isquote(*p))
                ++p;
            if (!strncmp(p, "set", 3))
            {
                p+=3;
                if (isquote(*p) || !*p)
                    default_set=1;
            }
            else if ((!strncmp(p, "no", 2) && (!p[3] || isquote(p[3]))) || !*p || isquote(*p))
            {
                p+=2;
                if (isquote(*p) || !*p)
                {
                    default_set = -1;
                }
            }
            else
            {
                //fprintf(stderr, "invalid value for CHECK_PERMISSIONS (must be 'set', 'warn' or 'no')\n");
            }
        }
#define FSCAPSENABLE "PERMISSION_FSCAPS="
        else if (!strncmp(p, FSCAPSENABLE, strlen(FSCAPSENABLE)))
        {
            p+=strlen(FSCAPSENABLE);
            if (isquote(*p))
            {
                ++p;
            }

            if (!strncmp(p, "yes", 3))
            {
                p+=3;
                if (isquote(*p) || !*p)
                {
                    *fscaps_configured=true;
                }
            }
            else if (!strncmp(p, "no", 2))
            {
                p+=2;
                if (isquote(*p) || !*p)
                {
                    *fscaps_configured=false;
                }
            }
        }
    }
    fclose(fp);
    return 0;
}

static int
compare(const void* a, const void* b)
{
    return strcmp(*(char* const*)a, *(char* const*)b);
}

static void
read_permissions_d(const char *directory)
{
    size_t i;
    DIR* dir;

    dir = opendir(directory);
    if (dir)
    {
        char** files = NULL;
        size_t nfiles = 0;
        struct dirent* d;
        while ((d = readdir(dir)))
        {
            char* p;
            if (!strcmp("..", d->d_name) || !strcmp(".", d->d_name))
                continue;

            /* filter out backup files */
            if ((strlen(d->d_name)>2) && (d->d_name[strlen(d->d_name)-1] == '~'))
                continue;
            if (strstr(d->d_name,".rpmnew") || strstr(d->d_name,".rpmsave"))
                continue;

            ensure_array((void**)&files, &nfiles);
            if ((p = strchr(d->d_name, '.')))
            {
                *p = '\0';
            }
            files[nfiles++] = strdup(d->d_name);
        }
        closedir(dir);
        if (nfiles)
        {
            qsort(files, nfiles, sizeof(char*), compare);
            for (i = 0; i < nfiles; ++i)
            {
                char fn[4096];
                size_t l;
                // skip duplicates
                if (i && !strcmp(files[i-1], files[i]))
                    continue;

                snprintf(fn, sizeof(fn), "%s/%s", directory, files[i]);
                if (access(fn, R_OK) == 0)
                {
                    ensure_array((void**)&permfiles, &npermfiles);
                    permfiles[npermfiles++] = strdup(fn);
                }

                for (l = 0; l < nlevel; ++l)
                {
                    snprintf(fn, sizeof(fn), "%s/%s.%s", directory, files[i], level[l]);

                    if (access(fn, R_OK) == 0)
                    {
                        ensure_array((void**)&permfiles, &npermfiles);
                        permfiles[npermfiles++] = strdup(fn);
                    }
                }

            }
            for (i = 0; i < nfiles; ++i)
            {
                free(files[i]);
            }
        }
        free(files);
    }
}

static void
collect_permfiles()
{
    size_t i;

    const auto usr_root = config_root + "/usr/share/permissions";
    const auto etc_root = config_root + "/etc";

    const auto central_usr_perms = usr_root + "/permissions";
    const auto central_etc_perms = etc_root + "/permissions";

    // 1. central fixed permissions file
    if (access(central_usr_perms.c_str(), R_OK) == 0)
    {
        ensure_array((void**)&permfiles, &npermfiles);
        permfiles[npermfiles++] = strdup(central_usr_perms.c_str());
    }
    else if (access(central_etc_perms.c_str(), R_OK) == 0)
    {
        ensure_array((void**)&permfiles, &npermfiles);
        permfiles[npermfiles++] = strdup(central_etc_perms.c_str());
    }

    // 2. central easy, secure paranoid as those are defined by SUSE
    for (i = 0; i < nlevel; ++i)
    {
        if (!strcmp(level[i], "easy")
                || !strcmp(level[i], "secure")
                || !strcmp(level[i], "paranoid"))
        {
            auto base = std::string("/permissions.") + level[i];
            for( const auto &dir: { usr_root, etc_root } )
            {
                std::string profile = dir + base;

                if (access(profile.c_str(), R_OK) == 0)
                {
                    ensure_array((void**)&permfiles, &npermfiles);
                    permfiles[npermfiles++] = strdup(profile.c_str());
                    break;
                }
            }
        }
    }

    // 3. package specific permissions
    read_permissions_d((usr_root + "/permissions.d").c_str());
    read_permissions_d((etc_root + "/permissions.d").c_str());

    // 4. central permissions files with user defined level incl 'local'
    for (i = 0; i < nlevel; ++i)
    {
        if (!strcmp(level[i], "easy") || !strcmp(level[i], "secure") || !strcmp(level[i], "paranoid"))
            continue;

        std::string profile = etc_root + "/permissions." + level[i];

        if (access(profile.c_str(), R_OK) == 0)
        {
            ensure_array((void**)&permfiles, &npermfiles);
            permfiles[npermfiles++] = strdup(profile.c_str());
        }
    }
}


void
usage(int x)
{
    printf("Usage:\n"
           "a) chkstat [OPTIONS] <permission-files>...\n"
           "b) chkstat --system [OPTIONS] <files>...\n"
           "\n"
           "Options:\n"
           "  --root DIR            check files relative to DIR\n"
           "  --config-root DIR     lookup config files relative to DIR\n"
    );
    exit(x);
}

enum proc_mount_state
{
    PROC_MOUNT_STATE_UNKNOWN,
    PROC_MOUNT_STATE_AVAIL,
    PROC_MOUNT_STATE_UNAVAIL,
};
static enum proc_mount_state proc_mount_avail = PROC_MOUNT_STATE_UNKNOWN;

static bool
check_have_proc(void)
{
    if (proc_mount_avail == PROC_MOUNT_STATE_UNKNOWN)
    {
        char *override = secure_getenv("CHKSTAT_PRETEND_NO_PROC");

        struct statfs proc;
        int r = statfs("/proc", &proc);
        if (override == NULL && r == 0 && proc.f_type == PROC_SUPER_MAGIC)
        {
            proc_mount_avail = PROC_MOUNT_STATE_AVAIL;
        }
        else
        {
            proc_mount_avail = PROC_MOUNT_STATE_UNAVAIL;
        }
    }

    return proc_mount_avail == PROC_MOUNT_STATE_AVAIL;
}

#define _STRINGIFY(s) #s
#define STRINGIFY(s) _STRINGIFY(s)

#define PROC_FD_PATH_SIZE (sizeof("/proc/self/fd/") + sizeof(STRINGIFY(INT_MAX)))

int
safe_open(char *path, struct stat *stb, uid_t target_uid, bool *traversed_insecure)
{
    char pathbuf[PATH_MAX];
    char *path_rest;
    int lcnt;
    int pathfd = -1;
    int parentfd = -1;
    struct stat root_st;
    bool is_final_path_element = false;

    *traversed_insecure = false;

    lcnt = 0;
    if ((size_t)snprintf(pathbuf, sizeof(pathbuf), "%s", path + rootl) >= sizeof(pathbuf))
        goto fail;

    path_rest = pathbuf;

    while (!is_final_path_element)
    {
        *path_rest = '/';
        char *cursor = path_rest + 1;

        if (pathfd == -1)
        {
            pathfd = open(rootl ? root : "/", O_PATH | O_CLOEXEC);

            if (pathfd == -1)
            {
                fprintf(stderr, "failed to open root directory %s: %s\n", root, strerror(errno));
                goto fail;
            }

            if (fstat(pathfd, &root_st))
            {
                fprintf(stderr, "failed to stat root directory %s: %s\n", root, strerror(errno));
                goto fail;
            }
            // stb and pathfd must be in sync for the root-escape check below
            memcpy(stb, &root_st, sizeof(*stb));
        }

        path_rest = strchr(cursor, '/');
        // path_rest is NULL when we reach the final path element
        is_final_path_element = path_rest == NULL || strcmp("/", path_rest) == 0;

        if (!is_final_path_element)
        {
            *path_rest = 0;
        }

        // multiple consecutive slashes: ignore
        if (!is_final_path_element && *cursor == '\0')
            continue;

        // never move up from the configured root directory (using the stat result from the previous loop iteration)
        if (strcmp(cursor, "..") == 0 && rootl && stb->st_dev == root_st.st_dev && stb->st_ino == root_st.st_ino)
            continue;

        // cursor is an empty string for trailing slashes, open again with different open_flags.
        int newpathfd = openat(pathfd, *cursor ? cursor : ".", O_PATH | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (newpathfd == -1)
            goto fail;

        close(pathfd);
        pathfd = newpathfd;

        if (fstat(pathfd, stb))
            goto fail;

        /* owner of directories must be trusted for setuid/setgid/capabilities as we have no way to verify file contents */
        /* for euid != 0 it is also ok if the owner is euid */
        if (stb->st_uid && stb->st_uid != euid && !is_final_path_element)
        {
            *traversed_insecure = true;
        }
        // path is in a world-writable directory, or file is world-writable itself.
        else if (!S_ISLNK(stb->st_mode) && (stb->st_mode & S_IWOTH) && !is_final_path_element)
        {
            *traversed_insecure = true;
        }

        // if parent directory is not owned by root, the file owner must match the owner of parent
        if (stb->st_uid && stb->st_uid != target_uid && stb->st_uid != euid)
        {
            if (is_final_path_element)
            {
                fprintf(stderr, "%s: has unexpected owner. refusing to correct due to unknown integrity.\n", path+rootl);
                goto fail;
            }
            else
                goto fail_insecure_path;
        }

        if (S_ISLNK(stb->st_mode))
        {
            // If the path configured in the permissions configuration is a symlink, we don't follow it.
            // This is to emulate legacy behaviour: old insecure versions of chkstat did a simple lstat(path) as 'protection' against malicious symlinks.
            if (is_final_path_element || ++lcnt >= 256)
                goto fail;

            // Don't follow symlinks owned by regular users.
            // In theory, we could also trust symlinks where the owner of the target matches the owner
            // of the link, but we're going the simple route for now.
            if (stb->st_uid && stb->st_uid != euid)
                goto fail_insecure_path;

            char linkbuf[PATH_MAX];
            ssize_t l = readlinkat(pathfd, "", linkbuf, sizeof(linkbuf) - 1);

            if (l <= 0 || (size_t)l >= sizeof(linkbuf) - 1)
                goto fail;

            while(l && linkbuf[l - 1] == '/')
            {
                l--;
            }

            linkbuf[l] = 0;

            if (linkbuf[0] == '/')
            {
                // absolute link
                close(pathfd);
                pathfd = -1;
            }
            else
            {
                // relative link: continue relative to the parent directory
                close(pathfd);
                if (parentfd == -1) // we encountered a link directly below /
                    pathfd = -1;
                else
                    pathfd = dup(parentfd);
            }

            // need a temporary buffer because path_rest points into pathbuf
            // and snprintf doesn't allow the same buffer as source and
            // destination
            char tmp[sizeof(pathbuf) - 1];
            size_t len = (size_t)snprintf(tmp, sizeof(tmp), "%s/%s", linkbuf, path_rest + 1);
            if (len >= sizeof(tmp))
                goto fail;

            // the first byte of path_rest is always set to a slash at the start of the loop, so we offset by one byte
            strcpy(pathbuf + 1, tmp);
            path_rest = pathbuf;
        }
        else if (S_ISDIR(stb->st_mode))
        {
            if (parentfd >= 0)
                close(parentfd);
            // parentfd is only needed to find the parent of a symlink.
            // We can't encounter links when resolving '.' or '..' so those don't need any special handling.
            parentfd = dup(pathfd);
        }
    }

    // world-writable file: error out due to unknown file integrity
    if (S_ISREG(stb->st_mode) && (stb->st_mode & S_IWOTH))
    {
        fprintf(stderr, "%s: file has insecure permissions (world-writable)\n", path+rootl);
        goto fail;
    }

    if (parentfd >= 0)
    {
        close(parentfd);
    }

    return pathfd;
fail_insecure_path:

    {
        char linkpath[PATH_MAX] = "ancestor";
        char procpath[PROC_FD_PATH_SIZE];
        snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", pathfd);
        ssize_t l = readlink(procpath, linkpath, sizeof(linkpath) - 1);
        if (l > 0)
        {
            linkpath[MIN((size_t)l, sizeof(linkpath) - 1)] = '\0';
        }
        fprintf(stderr, "%s: on an insecure path - %s has different non-root owner who could tamper with the file.\n", path+rootl, linkpath);
    }

fail:
    if (pathfd >= 0)
    {
        close(pathfd);
    }
    if (parentfd >= 0)
    {
        close(parentfd);
    }
    return -1;
}


/* check /sys/kernel/fscaps, 2.6.39 */
static int
check_fscaps_enabled()
{
    FILE* fp;
    char line[128];
    int val = FSCAPS_DEFAULT_ENABLED;

    if ((fp = fopen("/sys/kernel/fscaps", "r")) == 0)
    {
        goto out;
    }

    if (readline(fp, line, sizeof(line)))
    {
        val = atoi(line);
    }

    fclose(fp);
out:
    return val;
}

Chkstat::Chkstat(int argc, const char **argv) :
    m_argc(argc),
    m_argv(argv),
    m_parser("Tool to check and set file permissions"),
    m_system_mode("", "system", "system mode, act according to /etc/sysconfig/security", m_parser),
    m_force_fscaps("", "fscaps", "force use of file system capabilities", m_parser),
    m_disable_fscaps("", "no-fscaps", "disable use of file system capabilities", m_parser),
    m_apply_changes("s", "set", "actually apply changes (--system mode may imply this depending on file based configuration)", m_parser),
    m_only_warn("", "warn", "only inform about which changes would be performed but don't actually apply them (which is the default, except in --system mode)", m_parser),
    m_no_header("n", "noheader", "don't print intro message", m_parser),
    m_examine_paths("e", "examine", "operate only on the specified path(s)", false, "PATH", m_parser),
    m_force_level_list("", "level", "force application of the specified space-separated list of security levels (only supported in --system mode)", false, "", "e.g. \"local paranoid\"", m_parser),
    m_file_lists("f", "files", "read newline separated list of files to check (see --examine) from the specified path", false, "PATH", m_parser),
    m_root_path("r", "root", "check files relative to the given root directory", false, "", "PATH", m_parser),
    m_config_root_path("", "config-root", "lookup configuration files relative to the given root directory", false, "", "PATH", m_parser),
    m_input_args("args", "in --system mode a list of paths to check, otherwise a list of profiles to parse", false, "PATH", m_parser)
{
}

bool Chkstat::validateArguments()
{
    // exits on error/usage
    m_parser.parse(m_argc, m_argv);

    bool ret = true;

    const auto xor_args = {
        std::make_pair(&m_system_mode, static_cast<TCLAP::SwitchArg*>(&m_apply_changes)),
        {&m_force_fscaps, &m_disable_fscaps},
        {&m_apply_changes, &m_only_warn}
    };

    for (const auto &args: xor_args)
    {
        const auto &arg1 = *args.first;
        const auto &arg2 = *args.second;

        if (arg1.isSet() && arg2.isSet())
        {
            std::cerr << arg1.getName() << " and " << arg2.getName()
                << " cannot be set at the same time\n";
            ret = false;
        }
    }

    if (!m_system_mode.isSet() && m_input_args.getValue().empty())
    {
        std::cerr << "one or more permission file paths to use are required\n";
        ret = false;
    }

    for (const auto arg: { &m_root_path, &m_config_root_path })
    {
        if (!arg->isSet())
            continue;

        const auto &path = arg->getValue();

        if (path.empty() || path[0] != '/')
        {
            std::cerr << arg->getName() << " must begin with '/'\n";
            ret = false;
        }
    }

    if (m_config_root_path.isSet())
    {
        const auto &path = m_config_root_path.getValue();

        // considering NAME_MAX characters left is somewhat arbitrary, but
        // staying within these limits should at least allow us to not
        // encounter ENAMETOOLONG in typical setups
        if (path.length() >= (PATH_MAX - NAME_MAX -1))
        {
            std::cerr << m_config_root_path.getName() << ": prefix is too long\n";
            ret = false;
        }
    }

    return ret;
}

int Chkstat::run()
{
    char *str;
    bool use_checklist = false;
    bool use_fscaps = true;
    char line[512];
    char *part[4];
    int pcnt, lcnt;
    size_t i;
    int inpart;
    mode_t mode;
    struct perm *e;
    struct stat stb;
    struct passwd *pwd = 0;
    struct group *grp = 0;
    uid_t uid;
    gid_t gid;
    int fd = -1;
    int errors = 0;
    cap_t caps = NULL;

    if (!validateArguments())
    {
        return 2;
    }

    for (const auto &path: m_examine_paths.getValue())
    {
        add_checklist(path.c_str());
        use_checklist = true;
    }

    for (const auto &path: m_file_lists.getValue())
    {
        FILE *fp = fopen(path.c_str(), "r");
        if (fp == 0)
        {
            fprintf(stderr, "files: %s: %s\n", path.c_str(), strerror(errno));
            return 1;
        }
        while (readline(fp, line, sizeof(line)))
        {
            if (!*line)
                continue;
            add_checklist(line);
        }
        fclose(fp);
        use_checklist = true;
    }

    if (m_root_path.isSet())
    {
        const auto &rp = m_root_path.getValue();
        root = rp.c_str();
        rootl = rp.length();
    }

    if (m_config_root_path.isSet())
    {
        config_root = m_config_root_path.getValue();
    }

    if (m_system_mode.isSet())
    {
        const std::string file = config_root + "/etc/sysconfig/security";
        // use_fscaps will only be changed if explicitly configured
        parse_sysconf(file.c_str(), !m_force_level_list.isSet(), &use_fscaps);
        if (!m_apply_changes.isSet() && !m_only_warn.isSet())
        {
            if (default_set < 0)
            {
                fprintf(stderr, "permissions handling disabled in %s\n", file.c_str());
                exit(0);
            }
            m_apply_changes.setValue(default_set);
        }
        if (m_force_level_list.isSet())
        {
            std::istringstream ss(m_force_level_list.getValue());
            std::string word;
            while (std::getline(ss, word))
            {
                add_level(word.c_str());
            }
        }

        if (!nlevel)
            add_level("secure");
        add_level("local"); // always add local

        for (const auto &path: m_input_args.getValue())
        {
            add_checklist(path.c_str());
            use_checklist = true;
            continue;
        }
        collect_permfiles();
    }
    else
    {
        for (const auto &path: m_input_args.getValue())
        {
            ensure_array((void**)&permfiles, &npermfiles);
            permfiles[npermfiles++] = strdup(path.c_str());
        }
    }

    // apply possible command line overrides to force en-/disable fscaps
    if (m_force_fscaps.isSet())
    {
        use_fscaps = true;
    }
    else if (m_disable_fscaps.isSet())
    {
        use_fscaps = false;
    }

    if (use_fscaps && !check_fscaps_enabled())
    {
        fprintf(stderr, "Warning: running kernel does not support fscaps\n");
    }

    // add fake list entries for all files to check
    for (i = 0; i < nchecklist; i++)
        add_permlist(checklist[i], "unknown", "unknown", 0);

    for (i = 0; i < npermfiles; i++)
    {
        FILE *fp = fopen(permfiles[i], "r");
        if (fp == 0)
        {
            perror(permfiles[i]);
            exit(1);
        }
        lcnt = 0;
        struct perm* last = NULL;
        int extline;
        while (readline(fp, line, sizeof(line)))
        {
            extline = 0;
            lcnt++;
            if (*line == 0 || *line == '#' || *line == '$')
                continue;
            inpart = 0;
            pcnt = 0;
            char *p;
            for (p = line; *p; p++)
            {
                if (*p == ' ' || *p == '\t')
                {
                    *p = 0;
                    if (inpart)
                    {
                        pcnt++;
                        inpart = 0;
                    }
                    continue;
                }
                if (pcnt == 0 && !inpart && *p == '+')
                {
                    extline = 1;
                    break;
                }
                if (!inpart)
                {
                    inpart = 1;
                    if (pcnt == 3)
                        break;
                    part[pcnt] = p;
                }
            }
            if (extline)
            {
                if (!last)
                {
                    BAD_LINE();
                    continue;
                }
                if (!strncmp(p, "+capabilities ", 14))
                {
                    if (!use_fscaps)
                        continue;
                    p += 14;
                    caps = cap_from_text(p);
                    if (caps)
                    {
                        cap_free(last->caps);
                        last->caps = caps;
                    }
                    continue;
                }
                BAD_LINE();
                continue;
            }
            if (inpart)
                pcnt++;
            if (pcnt != 3)
            {
                BAD_LINE();
                continue;
            }
            part[3] = part[2];
            part[2] = strchr(part[1], ':');
            if (!part[2])
                part[2] = strchr(part[1], '.');
            if (!part[2])
            {
                BAD_LINE();
                continue;
            }
            *part[2]++ = 0;
            mode = (mode_t)strtoul(part[3], part + 3, 8);
            if (mode > 07777 || part[3][0])
            {
                BAD_LINE();
                continue;
            }
            last = add_permlist(part[0], part[1], part[2], mode);
        }
        fclose(fp);
    }

    euid = geteuid();
    for (e = permlist; e; e = e->next)
    {
        if (use_checklist && !in_checklist(e->file+rootl))
            continue;

        pwd = strcmp(e->owner, "unknown") ? getpwnam(e->owner) : NULL;
        grp = strcmp(e->group, "unknown") ? getgrnam(e->group) : NULL;
        uid = pwd ? pwd->pw_uid : 0;
        gid = grp ? grp->gr_gid : 0;

        bool traversed_insecure;
        if (fd >= 0)
        {
            // close fd from previous loop iteration
            close(fd);
            fd = -1;
        }

        fd = safe_open(e->file, &stb, uid, &traversed_insecure);
        if (fd < 0)
            continue;
        if (S_ISLNK(stb.st_mode))
            continue;

        if (!e->mode && !strcmp(e->owner, "unknown"))
        {
            fprintf(stderr, "%s: cannot verify ", e->file+rootl);
            pwd = getpwuid(stb.st_uid);
            if (pwd)
                fprintf(stderr, "%s:", pwd->pw_name);
            else
                fprintf(stderr, "%d:", stb.st_uid);
            grp = getgrgid(stb.st_gid);
            if (grp)
                fprintf(stderr, "%s", grp->gr_name);
            else
                fprintf(stderr, "%d", stb.st_gid);
            fprintf(stderr, " %04o - not listed in /etc/permissions\n",
                    (int)(stb.st_mode&07777));
            continue;
        }
        if (!pwd)
        {
            fprintf(stderr, "%s: unknown user %s. ignoring entry.\n", e->file+rootl, e->owner);
            continue;
        }
        else if (!grp)
        {
            fprintf(stderr, "%s: unknown group %s. ignoring entry.\n", e->file+rootl, e->group);
            continue;
        }

        // fd is opened with O_PATH, file oeprations like cap_get_fd() and fchown() don't work with it.
        //
        // We also don't want to do a proper open() of the file, since that doesn't even work for sockets
        // and might have side effects for pipes or devices.
        //
        // So we use path-based operations (yes!) with /proc/self/fd/xxx. (Since safe_open already resolved
        // all symlinks, 'fd' can't refer to a symlink which we'd have to worry might get followed.)
        char fd_path_buf[PROC_FD_PATH_SIZE];
        char *fd_path;
        if (check_have_proc())
        {
            snprintf(fd_path_buf, sizeof(fd_path_buf), "/proc/self/fd/%d", fd);
            fd_path = fd_path_buf;
        }
        else
            // fall back to plain path-access for read-only operation. (this much is fine)
            // below we make sure that in this case we report errors instead of trying to fix policy violations insecurely
            fd_path = e->file;

        caps = cap_get_file(fd_path);
        if (!caps)
        {
            // we get EBADF for files that don't support capabilities, e.g. sockets or FIFOs
            if (errno == EBADF)
            {
                if (e->caps)
                {
                    fprintf(stderr, "%s: cannot assign capabilities for this kind of file\n", e->file+rootl);
                    cap_free(e->caps);
                    errors++;
                }
                e->caps = NULL;
            }
            if (errno == EOPNOTSUPP)
            {
                if (e->caps)
                    cap_free(e->caps);
                e->caps = NULL;
            }
        }
        if (e->caps)
        {
            e->mode &= 0777;
        }

        int perm_ok = (stb.st_mode & 07777) == e->mode;
        int owner_ok = stb.st_uid == uid && stb.st_gid == gid;
        int caps_ok = 0;

        if (!caps && !e->caps)
            caps_ok = 1;
        else if (caps && e->caps && !cap_compare(e->caps, caps))
            caps_ok = 1;

        if (perm_ok && owner_ok && caps_ok)
            continue;

        if (!m_no_header.isSet())
        {
            printf("Checking permissions and ownerships - using the permissions files\n");
            for (i = 0; i < npermfiles; i++)
                printf("\t%s\n", permfiles[i]);
            if (rootl)
            {
                printf("Using root %s\n", root);
            }

            m_no_header.setValue(true);
        }

        if (m_apply_changes.isSet() && fd_path != fd_path_buf)
        {
            fprintf(stderr, "ERROR: /proc is not available - unable to fix policy violations.\n");
            errors++;
            m_apply_changes.setValue(false);
        }

        if (!m_apply_changes.isSet())
            printf("%s should be %s:%s %04o", e->file+rootl, e->owner, e->group, e->mode);
        else
            printf("setting %s to %s:%s %04o", e->file+rootl, e->owner, e->group, e->mode);

        if (!caps_ok && e->caps)
        {
            str = cap_to_text(e->caps, NULL);
            printf(" \"%s\"", str);
            cap_free(str);
        }
        printf(". (wrong");
        if (!owner_ok)
        {
            pwd = getpwuid(stb.st_uid);
            grp = getgrgid(stb.st_gid);
            if (pwd)
                printf(" owner/group %s", pwd->pw_name);
            else
                printf(" owner/group %d", stb.st_uid);
            if (grp)
                printf(":%s", grp->gr_name);
            else
                printf(":%d", stb.st_gid);
            pwd = 0;
            grp = 0;
        }

        if (!perm_ok)
            printf(" permissions %04o", (int)(stb.st_mode & 07777));

        if (!caps_ok)
        {
            if (!perm_ok || !owner_ok)
            {
                fputc(',', stdout);
            }
            if (caps)
            {
                str = cap_to_text(caps, NULL);
                printf(" capabilities \"%s\"", str);
                cap_free(str);
            }
            else
                fputs(" missing capabilities", stdout);
        }
        putchar(')');
        putchar('\n');

        if (!m_apply_changes.isSet())
            continue;

        // don't give high privileges to files controlled by non-root users
        if ((e->caps || (e->mode & S_ISUID) || (e->mode & S_ISGID)) && !S_ISREG(stb.st_mode) && !S_ISDIR(stb.st_mode))
        {
            fprintf(stderr, "%s: will only assign capabilities or setXid bits to regular files or directories\n", e->file+rootl);
            errors++;
            continue;
        }
        if (traversed_insecure && (e->caps || (e->mode & S_ISUID) || ((e->mode & S_ISGID) && S_ISREG(stb.st_mode))))
        {
            fprintf(stderr, "%s: will not give away capabilities or setXid bits on an insecure path\n", e->file+rootl);
            errors++;
            continue;
        }

        if (euid == 0 && !owner_ok)
        {
            /* if we change owner or group of a setuid file the bit gets reset so
               also set perms again */
            if (e->mode & (S_ISUID | S_ISGID))
                perm_ok = 0;
            if (chown(fd_path, uid, gid))
            {
                fprintf(stderr, "%s: chown: %s\n", e->file+rootl, strerror(errno));
                errors++;
            }
        }
        if (!perm_ok && chmod(fd_path, e->mode))
        {
            fprintf(stderr, "%s: chmod: %s\n", e->file+rootl, strerror(errno));
            errors++;
        }
        // chown and - depending on the file system - chmod clear existing capabilities
        // so apply the intended caps even if they were correct previously
        if (e->caps || !caps_ok)
        {
            if (S_ISREG(stb.st_mode))
            {
                // cap_set_file() tries to be helpful and does a lstat() to check that it isn't called on
                // a symlink. So we have to open() it (without O_PATH) and use cap_set_fd().
                int cap_fd = open(fd_path, O_NOATIME | O_CLOEXEC | O_RDONLY);
                if (cap_fd == -1)
                {
                    fprintf(stderr, "%s: open() for changing capabilities: %s\n", e->file+rootl, strerror(errno));
                    errors++;
                }
                else if (cap_set_fd(cap_fd, e->caps))
                {
                    // ignore ENODATA when clearing caps - it just means there were no caps to remove
                    if (errno != ENODATA || e->caps)
                    {
                        fprintf(stderr, "%s: cap_set_fd: %s\n", e->file+rootl, strerror(errno));
                        errors++;
                    }
                }
                if (cap_fd != -1)
                    close(cap_fd);
            }
            else
            {
                fprintf(stderr, "%s: cannot set capabilities: not a regular file\n", e->file+rootl);
                errors++;
            }
        }
    }
    // close fd from last loop iteration
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
    if (errors)
    {
        fprintf(stderr, "ERROR: not all operations were successful.\n");
        return 1;
    }

    for (i = 0; i < npermfiles; i++)
    {
        free(permfiles[i]);
    }
    free(permfiles);

    return 0;
}

int main(int argc, const char **argv)
{
    Chkstat chkstat(argc, argv);
    return chkstat.run();
}

// vim: et ts=4 sts=4 sw=4 :