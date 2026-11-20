/* Host entry for the recompiled Homeworld Remastered (HW2HD_Exe):
 *   hw2hd <path to HomeworldRemastered.app> [game arguments...]
 * The game's executable is read from the app for its data segments (the code is the recompiled copy linked into this
 * program). Environment switches: HW_TRACE=1 (log import calls), HW_STUB=1 (unimplemented imports return 0). */
#include "mac.h"
#include <sys/mman.h>
#include <unistd.h>

void hw_load(const char *exe);
int hw_run(int argc, char **argv);
extern char hw_exe_path[1024];

#include <signal.h>
#include <time.h>
/* Opened from Finder the runtime's messages would go nowhere: they go to <data>/userdata/hw2hd.log (the run before
 * kept as hw2hd.old.log), with the time each run started and how it ended. */
static void log_to_file(void)
{
    if (isatty(2) && !getenv("HW_LOG_FILE")) return;
    const char *hw_data_dir(void); char path[1100], old[1100];
    snprintf(path, sizeof path, "%s/hw2hd.log", hw_data_dir()); snprintf(old, sizeof old, "%s/hw2hd.old.log", hw_data_dir());
    rename(path, old);
    if (!freopen(path, "w", stderr)) return;
    setvbuf(stderr, 0, _IOLBF, 0);
    time_t t = time(0); char when[64]; strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(stderr, "hw: started %s\n", when);
}
static void on_signal(int sig)                          /* how a run ended, when the system ended it */
{
    char msg[96]; int n = snprintf(msg, sizeof msg, "hw: signal %d (%s): the game is stopping\n", sig, strsignal(sig));
    if (n > 0) write(2, msg, (size_t)n);
    signal(sig, SIG_DFL); raise(sig);
}
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <HomeworldRemastered.app> [arguments]\n", argv[0]); return 2; }
    log_to_file();
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGHUP, SIGTRAP};
    for (unsigned k = 0; k < sizeof sigs / sizeof sigs[0]; k++) signal(sigs[k], on_signal);
    rt_seh_scan_calls = 0;                              /* landing pads are entered only by the C++ unwinder */
    extern int rt_cpuid_sse2, rt_cpuid_sse3; rt_cpuid_sse2 = rt_cpuid_sse3 = 1;   /* a Core 2 era Mac */
    extern int hw_test_mode; hw_test_mode = getenv("HW_TEST") != 0;
    hw_trace = getenv("HW_TRACE") != 0; hw_stub = getenv("HW_STUB") != 0;
    snprintf(hw_game_dir, sizeof hw_game_dir, "%s/Contents", argv[1]);
    snprintf(hw_exe_path, sizeof hw_exe_path, "%s/MacOS/HW2HD_Exe", hw_game_dir);
    void *p = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("guest memory"); return 1; }
    G_MEM = p;
    hw_load(hw_exe_path);
    char wd[1100]; snprintf(wd, sizeof wd, "%s/Assets/HomeworldRM", hw_game_dir);   /* the engine finds Data/ from here */
    if (chdir(wd)) hw_log("cannot enter %s", wd);
    /* the game sees argv[0] = its executable inside the app, then the remaining arguments */
    char **gargv = calloc((size_t)argc, sizeof *gargv); gargv[0] = hw_exe_path;
    for (int k = 2; k < argc; k++) gargv[k - 1] = argv[k];
    return hw_run(argc - 1, gargv);
}
