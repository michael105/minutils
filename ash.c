/* Copyright 2018 - this program is licensed under the 2-clause BSD license
   see LICENSE for the full license info
*/

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <pwd.h>
#include <unistd.h>

#define PNAME "ash"
#define VERSION "0.0.1"
#define PROMPT "$"
#define ENV_HOME "HOME"
#define ENV_PATH "PATH"
#define ENV_ASH_HISTORY ".ash_history"
#define DEFAULT_UNAME "anon"
#define DEFAULT_HOST "unknown"
#define DEFAULT_PATH_SIZE 225
#define DEFAULT_HISTORY_SIZE 500
#define MAX_HOST_SIZE 64
#define MIN_BUFFER_SIZE 2096
#define MAX_BUFFER_SIZE 16384
#define MAX_ARGV 255

#define TRM_OFF "\x1b[0m"
#define TRM_BOLD "\x1b[1m"
#define TRM_WHITE "\x1b[37m"
#define TRM_GREEN "\x1b[32m"

static char *pwd = NULL;
static size_t pwd_size = DEFAULT_PATH_SIZE;
static char *dir = NULL;
static const char *home = NULL;
static const char *uname = DEFAULT_UNAME;
static const char *host = DEFAULT_HOST;
static const char *path = NULL;

static char buf[MIN_BUFFER_SIZE];

static void print(void);
static void ash_pwd(void);
static void ash_uname_host(void);
static void ash_print_help(void);
static void ash_print_builtin(void);
static int ash_find_builtin(const char *);
static void ash_print_builtin_info(int, const char *);

enum ash_builtin {
    EXIT,
    ECHO,
    SLEEP,
    CD,
    CLEAR,
    HELP,
    BUILTIN
};

static void print_err(const char *msg)
{
    fprintf(stdout, PNAME ": error: %s\n", msg);
}

static void print_errno(const char *msg)
{
    fprintf(stdout, PNAME ": error: %s: %s\n", msg, strerror(errno));
}

static void print_err_builtin(const char *pname, const char *msg)
{
    fprintf(stdout, PNAME " %s: error: %s \n", pname, msg);
}

enum ash_errno {
    ARG_MSG_ERR,
    TYPE_ERR,
    PARSE_ERR,
    UREG_CMD_ERR,
    SIG_ERR
};

static const char *perr(int o)
{
    switch (o){
        case ARG_MSG_ERR:     return "expected argument";
        case TYPE_ERR:        return "incorrect value type";
        case PARSE_ERR:       return "parsed with errors";
        case UREG_CMD_ERR:    return "unrecognized command";
        case SIG_ERR:         return "abnormal termination";
    }
    return NULL;
}

struct ash_variable {
    int id;
    const char *name;
    const char *value;
};

enum ash_builtin_variable {
    ASH_VERSION = 0,
    ASH_HOST = 1,
    ASH_PATH = 2,
    ASH_HOME = 3,
    ASH_PWD = 4,
    ASH_LOGNAME = 5
};

static struct ash_variable ash_vars[] = {
    {
        .id = ASH_VERSION,
        .name = "VERSION",
        .value = VERSION
    },

    {
        .id = ASH_HOST,
        .name = "HOST",
        .value = NULL
    },

    {
        .id = ASH_PATH,
        .name = "PATH",
        .value = NULL
    },

    {
        .id = ASH_HOME,
        .name = "HOME",
        .value = NULL
    },

    {
        .id = ASH_PWD,
        .name = "PWD",
        .value = NULL
    },

    {
        .id = ASH_LOGNAME,
        .name = "LOGNAME",
        .value = NULL
    }
};

static struct ash_variable *ash_find_builtin_var(int o)
{
    if (o < sizeof (ash_vars) / sizeof (ash_vars[0]))
        return &ash_vars[o];
    return NULL;
}

static struct ash_variable *ash_find_var(const char *s)
{
    const char *v = s;
    switch(*(v++)){
        case 'V':
            if (*(v++) == 'E' && *(v++) == 'R' && *(v++) == 'S' &&
               *(v++) == 'I' && *(v++) == 'O' && *(v++) == 'N' && !(*v))
                return ash_find_builtin_var(ASH_VERSION);
            break;

        case 'H':
            if (*(v++) == 'O'){
                if (*(v++) == 'S' && *(v++) == 'T' && !(*v))
                    return ash_find_builtin_var(ASH_HOST);
                else if (*(--v) && *(v++) == 'M' && *(v++) == 'E' && !(*v))
                    return ash_find_builtin_var(ASH_HOME);
            }
            break;

        case 'P':
            if (*(v++) == 'A' && *(v++) == 'T' && *(v++) == 'H' &&
               !(*v))
                return ash_find_builtin_var(ASH_PATH);
            else if (*(--v) && *(v++) == 'W' && *(v++) == 'D' && !(*v))
                return ash_find_builtin_var(ASH_PWD);
            break;

        case 'L':
            if(*(v++) == 'O' && *(v++) == 'G' && *(v++) == 'N' &&
               *(v++) == 'A' && *(v++) == 'M' && *(v++) == 'E' && !(*v))
                return ash_find_builtin_var(ASH_LOGNAME);
            break;
    }
    return NULL;
}

static void ash_dir(void){
    if (pwd){
        size_t len = strlen(pwd);
        while (len > 0)
            if (pwd[--len] == '/'){
                dir = &pwd[++len];
                return;
            }
    }
    dir = ".";
}

static void ash_builtin_exec(int o, int argc, const char * const*argv)
{
    int status;

    switch (o){
        case BUILTIN:
            if (argc == 1)
                ash_print_builtin();
            else {
                if ((status = ash_find_builtin(argv[1])) != -1)
                    ash_print_builtin_info(status, argv[1]);
                else
                    print_err_builtin(argv[1], perr(UREG_CMD_ERR));
            }
            break;

        case EXIT:
            exit(0);
            break;

        case ECHO:
            if(argc > 1){
                for (size_t i = 1; i < argc -1; ++i)
                    fprintf(stdout, "%s ", argv[i]);
                fputs(argv[argc -1], stdout);
                fputc('\n', stdout);
            }
            break;

        case SLEEP:
            if(argc == 1)
                print_err_builtin(argv[0], perr(ARG_MSG_ERR));
            else {
                status = 1;
                for (size_t i = 0; i < strlen(argv[1]); ++i){
                    char c = argv[1][i];
                    if (!(c <= '9' && c >= '0'))
                        status = 0;
                }
                if (status)
                    sleep(atoi(argv[1]));
                else
                    print_err_builtin(argv[0], perr(TYPE_ERR));
            }
            break;

        case CD:
            if (argc > 1){
                const char *s = argv[1];
                char home_dir[pwd_size];
                if (*s == '~'){
                    const char *v = s;
                    if (v[1] == '/')
                        ++v;
                    size_t pos = strlen(home);
                    memset(home_dir, 0, pwd_size);
                    strcpy(home_dir, home);
                    strcpy((home_dir + pos) + 1, ++v);
                    home_dir[pos] = '/';
                    s = home_dir;
                }
                status = chdir(s);
                if (status)
                    print_err_builtin(argv[0], strerror(errno));
                else {
                    ash_pwd();
                    struct ash_variable *ash_pwd = ash_find_builtin_var(ASH_PWD);
                    if (ash_pwd)
                        ash_pwd->value = pwd;
                    if (home && !strcmp(pwd, home))
                        dir = "~";
                }
            }
            break;

        case HELP:
            ash_print_help();
            break;
    }
}

static int ash_find_builtin(const char *v)
{
    switch (*(v++)){
        case 'b':
            if (*(v++) == 'u' && *(v++) == 'i' &&
                *(v++) == 'l' && *(v++) == 't' &&
                *(v++) == 'i' && *(v++) == 'n' && !(*v))
                return BUILTIN;
            break;
        case 'c':
            if (*(v++) == 'd' && !(*v))
                return CD;
            break;
        case 'e':
            if (*(v++) == 'x' && *(v++) == 'i' &&
               *(v++) == 't' && !(*v))
                return EXIT;
            else if(*(--v) && *(v++) == 'c' && *(v++) == 'h' &&
                    *(v++) == 'o' && !(*v))
                return ECHO;
            break;
        case 'h':
            if (*(v++) == 'e' && *(v++) == 'l' &&
                *(v++) == 'p' && !(*v))
                return HELP;
            break;
        case 's':
            if (*(v++) == 'l' && *(v++) == 'e' &&
                *(v++) == 'e' && *(v++) == 'p' && !(*v))
                return SLEEP;
            break;
    }
    return -1;
}

static int fcheck(const char *s){
    size_t pos = strlen(path);
    size_t len = strlen(s) + pos + 2;
    char arg[len];
    strcpy(arg, path);
    arg[pos] = '/';
    strcpy((arg + pos) + 1, s);
    arg[len] = '\0';

    if (access(arg, F_OK)){
        print_errno(s);
        return -1;
    }
    return 0;
}

static void execute(const char *p, char *const argv[])
{
    pid_t pid;
    int status;

    pid = fork();
    if (pid == -1)
        print_errno(argv[0]);
    else if (pid == 0){
        if (execvp(p, argv) == -1)
            print_errno(argv[0]);
    }
    else {
        wait(&status);
        if (WIFSIGNALED(status)){
            print_err_builtin(argv[0], perr(SIG_ERR));
            fprintf(stderr, "%s: exit status: %d\n", argv[0], WTERMSIG(status));
        }
    }
}

static void print(void)
{
    fprintf(stdout, TRM_BOLD TRM_GREEN "%s::%s " TRM_OFF TRM_BOLD "%s|" PROMPT TRM_OFF " " , uname, host, dir);
}

static int command(int argc, const char **argv)
{
    if (argc > 1)
        for (size_t i = 1; i < argc; ++i){
            const char *s = argv[i];
            if (*(s++) == '$'){
                struct ash_variable *var = ash_find_var(s);
                if(var && var->value)
                    argv[i] = var->value;
            }
        }

    const char *v = argv[0];
    if (*(v++) == '$') {
        struct ash_variable *var = ash_find_var(v);
        if (var && var->value)
            fprintf(stdout, "%s\n", var->value);
    } else {
        int o;
        if (( o = ash_find_builtin(argv[0])) != -1)
            ash_builtin_exec(o, argc, argv);
        else
            execute(argv[0], (char *const*)argv);
    }
    return 0;
}

static void scan(void)
{
    print();
    memset(buf, 0, MIN_BUFFER_SIZE);
    if (fgets(buf, MIN_BUFFER_SIZE, stdin)){
        int argc = 0;
        const char *argv[MAX_ARGV];
        memset(argv, 0, MAX_ARGV);
        if (!isspace(buf[0]))
            argv[argc++] = &buf[0];
        for (size_t i = 0; i < MIN_BUFFER_SIZE; ++i){
            if (buf[i] == '\n')
                buf[i] = '\0';
            if (isspace(buf[i])){
                buf[i] = '\0';
                if (!isspace(buf[i + 1]))
                    argv[argc++] = &buf[i + 1];
            }
        }
        if (!argc)
            return;
        command(argc, (const char **)argv);
    }
}

static void ash_pwd(void)
{
    pwd = getcwd(pwd, pwd_size);
    ash_dir();
}

static void ash_uname_host(void)
{
    uname = getpwuid(getuid())->pw_name;
    if (host)
        gethostname(host, MAX_HOST_SIZE);
}

static void ash_init(void)
{
    pwd_size = pathconf(".", _PC_PATH_MAX);
    if ((pwd = malloc(sizeof (char) * pwd_size)) != NULL)
        ash_pwd();

    uname = getpwuid(getuid())->pw_name;
    char *h = NULL;
    if ((h = malloc(sizeof (char) * MAX_HOST_SIZE)) != NULL){
        if(!gethostname(h, MAX_HOST_SIZE))
            host = h;
    }

    struct ash_variable *ash_host = ash_find_builtin_var(ASH_HOST);
    if (ash_host)
        ash_host->value = host;
    struct ash_variable *ash_path = ash_find_builtin_var(ASH_PATH);
    if (ash_path)
        ash_path->value = getenv(ENV_PATH);
    struct ash_variable *ash_home = ash_find_builtin_var(ASH_HOME);
    if (ash_home){
        if ((ash_home->value = getenv(ENV_HOME)) == NULL)
            ash_home->value = getpwuid(getuid())->pw_dir;
            home = ash_home->value;
        if (ash_home->value && !strcmp(pwd, ash_home->value))
                        dir = "~";
    }
    struct ash_variable *ash_pwd = ash_find_builtin_var(ASH_PWD);
    if (ash_pwd)
        ash_pwd->value = pwd;
    struct ash_variable *ash_logname = ash_find_builtin_var(ASH_LOGNAME);
    if (ash_logname)
        ash_logname->value = uname;
}

static void ash_main(int argc, const char **pargs)
{
    ash_init();

    for (;;){
        scan();
    }
}

static int ash_option(int argc, const char **argv)
{
    for (size_t i = 0; i < argc; i++)
        if (argv[i][0] == '-' && argv[i][1] == '-'){
            const char *s = &argv[i][2];
            if(!strcmp(s, "help"))
                ash_print_help();

            return 0;
        }
    return -1;
}

static void ash_print_builtin_info(int o, const char *s)
{
    printf( PNAME ": command: ");
    switch (o){
        case BUILTIN:
            printf("%s :: list builtin commands\n", s);
            break;

        case CD:
            printf("%s [dir] :: change directory\n", s);
            break;

        case ECHO:
            printf("%s :: print to stdout\n", s);
            break;

        case EXIT:
            printf("%s :: exit shell session\n", s);
            break;

        case HELP:
            printf("%s :: show usage info\n", s);
            break;

        case SLEEP:
            printf("%s [sec] :: sleep for [sec] seconds\n", s);
            break;
    }
}

static void ash_print_builtin(void)
{
    puts("list of builtin commands:");
    puts("type builtin [command] for more info\n");
    puts("builtin");
    puts("cd");
    puts("echo");
    puts("exit");
    puts("help");
    puts("sleep");
}

static void ash_print_help(void)
{
    fprintf(stdout, "ash: acorn shell %s\n", VERSION);
    fprintf(stdout, "usage: type commands e.g. help\n\n");
    fputs("        $$$      \n", stdout);
    fputs("         $$      \n", stdout);
    fputs("       $$$$$$    \n", stdout);
    fputs("     $$$$$$$$$$  \n", stdout);
    fputs("    $$$oooooo$$$ \n", stdout);
    fputs("    $$oooooooo$$ \n", stdout);
    fputs("     $oooooooo$  \n", stdout);
    fputs("      oooooooo   \n", stdout);
    fputs("        oooo     \n", stdout);
    fputc('\n', stdout);
}

int main(int argc, const char *argv[])
{
    if(ash_option(--argc, ++argv))
        ash_main(argc, argv);
    return 0;
}