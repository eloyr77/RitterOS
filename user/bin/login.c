/*
 * login: the program the kernel starts after the boot. It asks for a user's
 * name and password, then starts that user's shell (as that user, in the
 * home directory, as the leader of a session of its own, as a login shell:
 * `sh -l`, which runs ~/.config/login); when the shell ends, it asks again.
 * The user's own settings folder ~/.config is made at the login when it is
 * missing, with its files (README, login, windows; all comments until the
 * user changes them: 15l, the user's names). Ctrl-D at "login:" ends login, and the kernel's
 * rescue console follows. On a fresh system, where root has no password
 * yet, it first asks for one. Logins (and refused ones) and logouts go to
 * the log "login" (/system/logs/login.log, written by the elf logd, whose
 * elf file login makes when it is missing).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ritteros.h"
#include "settings_file.h"

PROGRAM_HELP("login", "log in: a user's name and password, then their shell");

#define PASSWORD_LENGTH 128

static char shell[160];

/* A typed line without its line break; false at the end of input (Ctrl-D).
 * Ctrl-C (an interrupted read) gives an empty line. */
static bool ask(const char *prompt, char *out, size_t size, bool hidden)
{
    printf("%s", prompt);
    if (hidden)
        ritteros_echo(false);
    ssize_t n = read(STDIN_FILENO, out, size - 1);
    if (hidden) {
        ritteros_echo(true);
        printf("\n");                               /* the Enter that was not shown */
    }
    if (n < 0 && errno == EINTR) {
        out[0] = 0;
        if (!hidden)
            printf("\n");
        return true;
    }
    if (n <= 0)
        return false;
    out[n] = 0;
    if (out[n - 1] == '\n')
        out[n - 1] = 0;
    return true;
}

/* A fresh system: root chooses a password first. */
static bool first_start(void)
{
    char first[PASSWORD_LENGTH], second[PASSWORD_LENGTH];
    printf("RitterOS: first start. Choose a password for root, the administrator.\n");
    for (;;) {
        if (!ask("New password for root: ", first, sizeof(first), true) ||
            !ask("Again: ", second, sizeof(second), true))
            return false;
        if (first[0] && strcmp(first, second) == 0)
            break;
        printf(first[0] ? "The two differ; once more.\n" : "An empty password is not allowed.\n");
    }
    int r = password_set("root", first);
    memset(first, 0, sizeof(first));
    memset(second, 0, sizeof(second));
    if (r < 0) {
        printf("login: %s: %s\n", PASSWORDS_FILE, strerror(errno));
        return false;
    }
    printf("The password for root is set.\n");
    return true;
}

/* (68j) After the first start's password: a system disk whose table was
 * made for a smaller disk (a stick image written to a bigger stick) gets
 * the room at its end: disks grow, its lines on the console. Nothing is
 * done (or printed) when the table fills the disk. */
static void grow_system_disk(void)
{
    struct sys_device d;
    bool small = false;
    for (int i = 0; !small && ritteros_device(i, &d) == 1; i++) {
        uint64_t array = (128 * 128 + d.block_size - 1) / (d.block_size ? d.block_size : 512);
        small = (d.flags & (DEVICE_SYSTEM | DEVICE_PARTITION)) == DEVICE_SYSTEM &&
                d.table == DEVICE_TABLE_GPT && d.block_count > array + 2 &&
                d.last_usable < d.block_count - 2 - array;
    }
    if (!small)
        return;
    char path[160];
    snprintf(path, sizeof(path), "%.*sdisks", (int)(strlen(shell) - 2), shell);
    char *argv[] = { "disks", "grow", 0 };
    int fds[3] = { -1, -1, -1 };
    pid_t pid = ritteros_spawn(path, argv, SPAWN_NEW_GROUP | SPAWN_FOREGROUND, fds);
    int status;
    while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    tcsetpgrp(STDIN_FILENO, getpgrp());
}

/* Disks from before step 32 keep users and passwords in /system: they move
 * to /system/config (a rename: nothing is lost on the way). */
static void move_old_files(void)
{
    static const char *const names[] = { "users", "passwords" };
    struct stat st;
    for (int i = 0; i < 2; i++) {
        char old[64], now[64];
        snprintf(old, sizeof(old), "/system/%s", names[i]);
        snprintf(now, sizeof(now), "%s/%s", CONFIG_DIR, names[i]);
        if (stat(old, &st) < 0 || stat(now, &st) == 0)
            continue;
        if (stat(CONFIG_DIR, &st) < 0)
            mkdir(CONFIG_DIR, 0755);
        if (rename(old, now) == 0)
            printf("login: %s moved to %s\n", old, now);
        else
            printf("login: %s: %s\n", old, strerror(errno));
    }
}

/* (38c) Does /system/config/network/virtual name a tap or a bridge (a line
 * that is not a comment)? Then net up makes them, cards or not. */
static bool virtual_wanted(void)
{
    FILE *f = fopen(SETTINGS_VIRTUAL, "r");
    if (!f)
        return false;
    char line[160];
    bool any = false;
    while (!any && fgets(line, sizeof(line), f)) {
        const char *k = line + strspn(line, " \t");
        any = *k != '#' && strchr(k, '=');
    }
    fclose(f);
    return any;
}

/* If there is a network card, or (38c) the virtual cards' file names one:
 * net up (the settings in /system/config/network/, else DHCP), next to the
 * shell. */
static void network_up(void)
{
    static char cards[1024];
    if (ritteros_report(REPORT_NET, cards, sizeof(cards) - 1) < 0 ||
        (strstr(cards, "\nno network cards\n") && !virtual_wanted()))
        return;                                     /* nothing to set up */
    char path[160];
    snprintf(path, sizeof(path), "%.*snet", (int)(strlen(shell) - 2), shell);
    char *argv[] = { "net", "up", 0 };
    int fds[3] = { -1, -1, -1 };
    pid_t pid = ritteros_spawn(path, argv, SPAWN_NEW_GROUP | SPAWN_FOREGROUND, fds);
    int status;
    while (pid > 0 && waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    tcsetpgrp(STDIN_FILENO, getpgrp());
}

/* The elf logd keeps the logs: its file, made if missing (the first start
 * of a system; logs are written at every boot, as the user chose). */
static void logd_ready(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/logd", ELVES_DIR);
    struct stat st;
    if (stat(path, &st) == 0)
        return;
    if (stat(ELVES_DIR, &st) < 0 && mkdir(ELVES_DIR, 0755) < 0) {
        printf("login: %s: %s\n", ELVES_DIR, strerror(errno));
        return;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    static const char text[] = "# The logs' keeper, an elf (login made this file)\n"
                               "command = logd\nstart = on\n";
    if (fd < 0 || write(fd, text, sizeof(text) - 1) != (ssize_t)sizeof(text) - 1) {
        printf("login: %s: %s\n", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return;
    }
    close(fd);
    printf("login: %s made: the elf logd keeps the logs in /system/logs\n", path);
}

/* (50h, the user's) The elf netnamesd, on every machine, also without a
 * network card: a card plugged in later is named, recorded and gets its
 * settings (and so its address) without net up. Its file, made if missing. */
static void netnamesd_ready(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/netnamesd", ELVES_DIR);
    struct stat st;
    if (stat(path, &st) == 0)
        return;
    if (stat(ELVES_DIR, &st) < 0 && mkdir(ELVES_DIR, 0755) < 0)
        return;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    static const char text[] = "# The network cards' names, an elf (login made this file)\n"
                               "command = netnamesd\nstart = on\n";
    if (fd < 0 || write(fd, text, sizeof(text) - 1) != (ssize_t)sizeof(text) - 1) {
        printf("login: %s: %s\n", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return;
    }
    close(fd);
    printf("login: %s made: the elf netnamesd records new network cards\n", path);
}

/* A line for the log "login", with what the terminal is. */
static void log_line(const char *what)
{
    char line[160];
    snprintf(line, sizeof(line), "%s on the console\n", what);
    ritteros_log_write("login", line);
}

/* If there are elves (files in /system/config/elves): elvesd, which starts
 * them and keeps them running, unless it runs already; not waited for. */
static void elves_up(void)
{
    DIR *dir = opendir(ELVES_DIR);
    if (!dir)
        return;
    bool any = false;
    struct dirent *entry;
    while (!any && (entry = readdir(dir)))
        any = entry->d_type == DT_REG && entry->d_name[0] != '.';
    closedir(dir);
    if (!any)
        return;
    char bin_dir[160];
    snprintf(bin_dir, sizeof(bin_dir), "%.*s", (int)(strlen(shell) - 3), shell);
    if (elvesd_start(bin_dir) < 0)                  /* (net up may have started it) */
        printf("login: %s/elvesd: %s\n", bin_dir, strerror(errno));
}

/* The user's own settings: ~/.config, the user's, with a README saying
 * what goes there and the files themselves, every line a comment until the
 * user changes it (made when missing: a new user's first login, or a user
 * from before; a file the user deleted comes back). */
static void make_user_config(const char *home, uid_t uid, gid_t gid)
{
    static const char readme[] =
        "Your own settings, read by the programs you run. /system/config holds\n"
        "the system's, for everyone; a file here wins over the system's file of\n"
        "the same name. Lines starting with # are comments: remove the # to use\n"
        "a line.\n"
        "\n"
        "login     commands run when you log in at the machine's own screen and\n"
        "          keyboard (not over SSH), one per line, before the first prompt.\n"
        "          A line `windowd` makes the window system your interface: when\n"
        "          you leave it, the shell's prompt is there.\n"
        "windows   the window system's settings, as /system/config/windows:\n"
        "          startup = PROGRAM [ARGUMENTS]   what windowd opens when it\n"
        "          starts (owl if not set; empty: nothing); double_click = MS\n"
        "          the time for a double click (500 if not set); click_steady =\n"
        "          off: clicks exactly where a touchpad's creep moved the pointer;\n"
        "          refresh = auto or frames a second (10-1000).\n"
        "filetypes which program a double click in owl (and open FILE) starts\n"
        "          for a file, by its ending: png = imageview, txt = edit...\n";
    static const char login_file[] =
        "# Commands run when you log in at this machine's screen and keyboard\n"
        "# (not over SSH), one per line, before the first prompt.\n"
        "#\n"
        "# The window system as your interface (remove the # below):\n"
        "# windowd\n";
    static const struct { const char *name, *text; size_t size; } files[] = {
        { "README", readme, sizeof(readme) - 1 },
        { "login", login_file, sizeof(login_file) - 1 },
        { "windows", SETTINGS_USER_WINDOWS_TEXT, sizeof(SETTINGS_USER_WINDOWS_TEXT) - 1 },
        { "filetypes", SETTINGS_USER_FILETYPES_TEXT, sizeof(SETTINGS_USER_FILETYPES_TEXT) - 1 },   /* (42a) */
    };
    char dir[160], path[192];
    snprintf(dir, sizeof(dir), "%s/.config", home);
    struct stat st;
    if (stat(dir, &st) < 0 && (mkdir(dir, 0700) < 0 || chown(dir, uid, gid) < 0)) {
        printf("login: %s: %s\n", dir, strerror(errno));
        return;
    }
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, files[i].name);
        if (stat(path, &st) == 0)
            continue;
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, files[i].text, files[i].size);
            close(fd);
            chown(path, uid, gid);
        }
    }
}

/* Start the user's shell and wait for it to end. */
static void run_shell(const struct passwd *pw)
{
    char name[USER_NAME_MAX + 1], home[128];
    snprintf(name, sizeof(name), "%s", pw->pw_name);
    snprintf(home, sizeof(home), "%s", pw->pw_dir);
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;
    struct stat st;
    if (stat(home, &st) < 0 && uid == 0) {          /* root's home, on first use */
        if (stat("/home", &st) < 0)
            mkdir("/home", 0755);
        mkdir(home, 0700);
    }
    if (chdir(home) < 0) {
        printf("login: %s: %s; starting in /\n", home, strerror(errno));
        chdir("/");
    } else {
        make_user_config(home, uid, gid);
    }
    setenv("HOME", home, 1);                        /* (22t: the session's environment) */
    setenv("USER", name, 1);
    setenv("LOGNAME", name, 1);
    setenv("SHELL", shell, 1);
    char *argv[] = { "sh", "-l", 0 };              /* a login shell: ~/.config/login */
    int fds[3] = { -1, -1, -1 };
    pid_t pid = ritteros_spawn_as(shell, argv,
                                  SPAWN_NEW_GROUP | SPAWN_NEW_SESSION | SPAWN_FOREGROUND, fds,
                                  uid, gid);
    chdir("/");
    if (pid < 0) {
        printf("login: %s: %s\n", shell, strerror(errno));
        return;
    }
    printf("[%s logged in]\n", name);
    char what[64];
    snprintf(what, sizeof(what), "%s logged in", name);
    log_line(what);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    tcsetpgrp(STDIN_FILENO, getpgrp());             /* the terminal is login's again */
    printf("[%s logged out]\n", name);
    snprintf(what, sizeof(what), "%s logged out", name);
    log_line(what);
}

int main(int argc, char **argv)
{
    (void)argc;
    /* The shell is next to login: /system/<arch>/bin/sh (the kernel starts
     * login by its name alone). */
    const char *slash = strrchr(argv[0], '/');
    if (slash)
        snprintf(shell, sizeof(shell), "%.*s/sh", (int)(slash - argv[0]), argv[0]);
    else
        snprintf(shell, sizeof(shell), "/system/%s/bin/sh",
#if defined(__x86_64__)
                 "x86_64"
#else
                 "aarch64"
#endif
        );
    signal(SIGINT, SIG_IGN);
    move_old_files();
    /* (37e-1b) The settings files nothing else writes: the library reads
     * time and services, sudo its file; missing ones get their defaults. */
    settings_make(SETTINGS_TIME, SETTINGS_TIME_TEXT);
    settings_make(SETTINGS_SERVICES, SETTINGS_SERVICES_TEXT);
    settings_make(SETTINGS_SUDO, SETTINGS_SUDO_TEXT);
    logd_ready();
    netnamesd_ready();
    network_up();
    elves_up();
    if (!password_exists("root")) {
        if (!first_start())
            return 1;
        grow_system_disk();                         /* (68j) */
    }
    char name[64], password[PASSWORD_LENGTH];
    for (;;) {
        if (!ask("\nlogin: ", name, sizeof(name), false)) {
            printf("\n");
            return 0;                               /* Ctrl-D: the rescue console */
        }
        if (!name[0])
            continue;
        if (!ask("Password: ", password, sizeof(password), true))
            return 0;
        struct passwd *pw = user_name_valid(name) ? getpwnam(name) : 0;
        /* Unknown users take as long as wrong passwords. */
        bool ok = password_check(pw ? name : "", password) == 1 && pw;
        memset(password, 0, sizeof(password));
        if (!ok) {
            printf("Login incorrect.\n");
            char what[96];
            snprintf(what, sizeof(what), "%s refused (a wrong name or password)",
                     user_name_valid(name) ? name : "a login");
            log_line(what);
            continue;
        }
        run_shell(pw);
    }
}
