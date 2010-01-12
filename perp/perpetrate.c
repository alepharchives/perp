/* perpetrate.c
** perpetrate: persistent process supervisor
** wcm, 2008.01.03 - 2010.01.01
** ===
*/

/* stdlib: */
#include <stdlib.h>
/* rename() declaration in stdio.h: */
extern int     rename(const char *oldpath, const char *newpath);

/* unix: */
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

/* lasagna headers: */
#include "buf.h"
#include "cstr.h"
#include "fd.h"
#include "nextopt.h"
#include "nfmt.h"
#include "outvec.h"
#include "pidlock.h"
#include "pkt.h"
#include "sig.h"
#include "sigset.h"
#include "sysstr.h"
#include "tain.h"
#include "uchar.h"
#include "upak.h"

/* macros for logging perp daemons to stderr: */
#include "loggit.h"
/* common defines for perp system: */
#include "perp_common.h"
#include "perplib.h"

/* access environment: */
extern char  **environ;

/*
** logging variables in scope:
*/
static const char *progpid = NULL;
static const char *progname = NULL;
static const char prog_usage[] = " [-hV] [-o] svdir";

/*
** configure loggit.h macros:
*/
#define LOGGIT_FMT \
  progname, "[", progpid, "] ", svdir, ": "

/*
** other variables in scope:
*/
static pid_t   mypid = 0;
static const char *svdir = NULL;
static const char *ctlpath = NULL;
static char    binstat_bin[] = "./" PERP_CONTROL "/" SV_DEVINO "/" STATUS_BIN;
static char    binstat_tmp[] = "./" PERP_CONTROL "/" SV_DEVINO "/" STATUS_TMP;

/* status update flag: */
static int     statchange = 0;

/* sigterm flag: */
static int     flagexit = 0;

/* selfpipe used for no race signal handling: */
static int     selfpipe[2];


/*
** objects in scope:
**
** nomenclature:
**
**   "super": perpetrate supervisor process (this be us)
**   "subsv": subservice process, main or log
*/


/* subsv subservice execve() targets: */
enum runwhat_t {
   RUN_START = 0,
   RUN_RESET = 1
};

/* subsv subservice: either SUBSV_MAIN or SUBSV_LOG */
struct subsv {
   pid_t          pid;          /* 0 if not running, else "start" or "reset" pid */
   int            isreset;      /* true if current non-zero pid is from "reset" */
   tain_t         when;         /* timestamp of most recent pid event */
   tain_t         when_ok;      /* timestamp for respawn governor */
   int            wstat;        /* wstat for exit from "start" */
   int            wantdown;
   int            isonce;
   int            ispaused;
   int            islog;
};
typedef struct subsv subsv_t;

/* super: perpetrate supervisor for subservice pair */
struct super {
   pid_t          pid;          /* my pid */
   tain_t         when;         /* my uptime */
   sigset_t       sigset;       /* signals to block/unblock during poll() */
   int            fdsvdir;      /* fchdir() for service definition directory */
   int            fdpidlock;    /* fd for pidlock file */
   int            fdfifo_0;     /* fd for input fifo */
   int            fdfifo_1;     /* fd for output fifo */
   int            fdctllock;    /* fd for client lock file */
   int            haslog;
   int            logpipe[2];
   int            flagdown;
   int            flagonce;
   /* buffer maintaining binary encoded service status: */
   binstat_t      binstat;
   /* subservice pair (main/log): */
   subsv_t        subsv[2];
};


/*
** prototypes in scope:
*/
static void    selfpipe_trigger(void);
static void    sig_handler(int sig);
static void    super_init(struct super *sv);
static void    subsv_init(struct super *sv, subsv_id which);
static void    setup_selfpipe(void);
static void    setup_control(struct super *sv);
static void    setup_service(struct super *sv);
static void    service_boot(struct super *sv);
static void    binstat_setflags(struct super *sv);
static void    binstat_post(struct super *sv);
static void    subsv_exec(struct super *sv, subsv_id which,
                          enum runwhat_t what);
static void    do_kill(struct super *sv, subsv_id which, int sig);
static int     do_control(struct super *sv, subsv_id which, uchar_t cmd);
static void    proto_error(int fd, int err);
static void    proto_status(struct super *sv, int fd);
static void    check_control(struct super *sv);
static void    check_children(struct super *sv);
static int     shut_down(struct super *sv);
static void    main_loop(struct super *sv);


/*
** procedure definitions:
*/


/*
** selfpipe trigger for poll() interrupt:
*/
static
void selfpipe_trigger(void)
{
   int            terrno = errno;
   int            w = 0;

   do {
      w = write(selfpipe[1], "!", 1);
   } while ((w == -1) && (errno == EINTR));

   errno = terrno;
   return;
}


/*
** signal handler (installed for SIGTERM, SIGCHLD):
*/
static
void sig_handler(int sig)
{
   switch (sig) {
   case SIGTERM:
      flagexit = 1;
      break;
   case SIGCHLD:
      break;
   default:
      return;
   }

   selfpipe_trigger();
   return;
}


/*
** object initializations:
*/

static
void super_init(struct super *sv)
{
   sv->pid = mypid;
   tain_now(&sv->when);
   sigset_empty(&sv->sigset);
   sv->fdsvdir = -1;
   sv->fdpidlock = -1;
   sv->fdfifo_0 = -1;
   sv->fdfifo_1 = -1;
   sv->fdctllock = -1;
   sv->haslog = 0;
   sv->logpipe[0] = -1;
   sv->logpipe[1] = -1;
   sv->flagdown = 0;
   sv->flagonce = 0;

   /* binstat initialization (block pertaining to supervisor): */
   buf_zero(sv->binstat, sizeof(binstat_t));
   upak32_pack(&sv->binstat[0], sv->pid);
   tain_pack(&sv->binstat[4], &sv->when);
   tain_pack(&sv->binstat[22], &sv->when);
   tain_pack(&sv->binstat[40], &sv->when);

   subsv_init(sv, SUBSV_MAIN);
   subsv_init(sv, SUBSV_LOG);

   return;
}


static
void subsv_init(struct super *sv, subsv_id which)
{
   subsv_t       *subsv = &sv->subsv[which];

   subsv->pid = 0;
   subsv->isreset = 0;
   tain_assign(&subsv->when, &sv->when);
   tain_load(&subsv->when_ok, 0, 0);
   subsv->wstat = 0;
   subsv->wantdown = 0;
   subsv->isonce = 0;
   subsv->ispaused = 0;
   subsv->islog = (which == SUBSV_MAIN ? 0 : 1);

   return;
}


/*
**   setup_*(): one-time initialization procedures called from main()
*/
static
void setup_selfpipe(void)
{
   /* setup selfpipe: */
   if (pipe(selfpipe) != 0) {
      fatal_syserr("failure pipe() creating selfpipe");
   }
   fd_cloexec(selfpipe[0]);
   fd_cloexec(selfpipe[1]);
   fd_nonblock(selfpipe[0]);
   fd_nonblock(selfpipe[2]);

   return;
}

/* setup_control()
**   initialize the shadow control directory for svdir
**   on entry: cwd is base directory
**   on exit:  cwd is base directory
**
**   errors on setup are fatal
*/
static
void setup_control(struct super *sv)
{
   int            fd_base, fd;
   struct stat    st;
   char           pidlock_path[] =
       "./" PERP_CONTROL "/" SV_DEVINO "/" PIDLOCK;
   ssize_t        w;

   log_debug("initializing service control directory ...");

   if (chdir(".") != 0) {
      fatal_syserr("failure chdir() to base directory");
   }
   if ((fd_base = open(".", O_RDONLY)) == -1) {
      fatal_syserr("failure open() on base directory");
   }

   /* stat the svdir argument: */
   if ((stat(svdir, &st)) == -1) {
      fatal_syserr("failure stat() on service definition directory");
   }
   if (!S_ISDIR(st.st_mode)) {
      fatal_usage("argument is not a directory: ", svdir);
   }

   /* path to service control directory (relative to base directory): */
   ctlpath = perp_ctlpath(&st);

   /* initialize control directory: */
   if (mkdir(ctlpath, 0700) == -1) {
      if (errno != EEXIST) {
         fatal_syserr("failure mkdir() for service control directory");
      }
   }

   /* acquire pidlock (assure single server instance): */
   cstr_vcopy(pidlock_path, ctlpath, "/" PIDLOCK);
   fd = pidlock_set(pidlock_path, mypid, PIDLOCK_NOW);
   if (fd == -1) {
      fatal_syserr("failure acquiring pidlock in service control directory");
   }
   fd_cloexec(fd);
   sv->fdpidlock = fd;

   /* first binstat: */
   cstr_vcopy(binstat_bin, ctlpath, "/" STATUS_BIN);
   cstr_vcopy(binstat_tmp, ctlpath, "/" STATUS_TMP);
   if ((fd = open(binstat_tmp, (O_WRONLY | O_CREAT | O_TRUNC))) == -1) {
      fatal_syserr("failure open() on service status file");
   }
   do {
      w = write(fd, sv->binstat, sizeof(binstat_t));
   } while ((w == -1) && (errno == EINTR));
   if (w == -1) {
      fatal_syserr("failure write() on service status file");
   }
   if (w < sizeof(binstat_t)) {
      fatal(111, "short write() initializing service status file");
   }
   close(fd);
   if (rename(binstat_tmp, binstat_bin) == -1) {
      fatal_syserr("failure rename() initializing service status file");
   }

   /* chdir() and complete population of control directory: */
   if (chdir(ctlpath) == -1) {
      fatal_syserr("failure chdir() to service control directory");
   }

   /* initialize client lock file (for cooperative single concurrency): */

   /* portablility comment:
    **
    ** In the original development on linux, we found we could use fcntl()
    ** locking on the CTL_IN fifo to implement cooperative single client
    ** concurrency.
    **
    ** On netbsd, however, we found that fcntl locking is not supported on fifos.
    ** The absence of fcntl locking on fifos also seems reasonable, even if it is
    ** supported on some systems.
    **
    ** So now we setup a regular file dedicated to this purpose...
    */
   if ((fd = open(CTL_LOCK, (O_RDWR | O_NONBLOCK | O_CREAT), 0600)) == -1) {
      fatal_syserr("failure open() on client lock file ", CTL_LOCK);
   }
   fd_cloexec(fd);
   sv->fdctllock = fd;

   /* initialize control fifos:
    **   note: CTL_IN is also used by clients to test status active/ok
    **   (so last step of setup)
    **/
   mkfifo(CTL_OUT, 0600);
   if ((fd = open(CTL_OUT, (O_RDWR | O_NONBLOCK))) == -1) {
      fatal_syserr("failure open() on output fifo ", CTL_OUT);
   }
   fd_cloexec(fd);
   sv->fdfifo_1 = fd;

   mkfifo(CTL_IN, 0600);
   if ((fd = open(CTL_IN, (O_RDWR | O_NONBLOCK))) == -1) {
      fatal_syserr("failure open() on input fifo ", CTL_IN);
   }
   fd_cloexec(fd);
   sv->fdfifo_0 = fd;

   /* setup of control directory complete: */
   if (fchdir(fd_base) == -1) {
      fatal_syserr("failure fchdir() on return from control directory");
   }
   close(fd_base);

   return;
}


/* setup_service()
**   inspect service definition directory and check configuration flags
**   executed on startup initialization
**   on entry: cwd is base directory
**   on exit:  cwd is base directory
**
**   errors on setup are fatal
*/
static
void setup_service(struct super *sv)
{
   struct stat    st;
   int            fd, fd_base;

   log_debug("initializing service definition directory ...");

   if (chdir(".") != 0) {
      fatal_syserr("failure chdir() to base directory");
   }
   if ((fd_base = open(".", O_RDONLY)) == -1) {
      fatal_syserr("failure open() on base directory");
   }

   if (chdir(svdir) != 0) {
      fatal_syserr("failure chdir() to service definition directory ", svdir);
   }
   if ((fd = open(".", O_RDONLY)) == -1) {
      fatal_syserr("failure open() on service definition directory ", svdir);
   }
   fd_cloexec(fd);
   sv->fdsvdir = fd;

   if (stat("flag.down", &st) != -1) {
      sv->flagdown = 1;
   }

   if (stat("flag.once", &st) != -1) {
      sv->flagonce = 1;
   }

   if (stat("rc.log", &st) != -1) {
      log_debug("rc.log exists");
      if (st.st_mode & S_IXUSR) {
         sv->haslog = 1;
         log_debug("rc.log exists and is executable, setting haslog=1");
      } else {
         log_warning("rc.log exists but is not executable");
      }
   }

   if (sv->haslog) {
      if (pipe(sv->logpipe) != 0) {
         fatal_syserr("failure pipe() creating logpipe");
      }
      fd_cloexec(sv->logpipe[0]);
      fd_cloexec(sv->logpipe[1]);
   }

   /* setup of service directory complete: */
   if (fchdir(fd_base) == -1) {
      fatal_syserr("failure fchdir() on return from service directory");
   }
   close(fd_base);

   return;
}


/* service_boot():
**   first time startup of services
*/
static
void service_boot(struct super *sv)
{
   /* if log exists, start irrespective of flags: */
   if (sv->haslog == 1) {
      subsv_exec(sv, SUBSV_LOG, RUN_START);
   }

   /* main: */
   if (sv->flagdown != 1) {
      /* setup for running once? */
      if (sv->flagonce == 1) {
         sv->subsv[SUBSV_MAIN].isonce = 1;
      }

      subsv_exec(sv, SUBSV_MAIN, RUN_START);
   } else {
      sv->subsv[SUBSV_MAIN].wantdown = 1;
   }

   return;
}


/* binstat_setflags()
**   update bitset flags in binary-encoded status
*/
static
void binstat_setflags(struct super *sv)
{
   subsv_id       which;
   uchar_t        flags;

   /* flags byte for supervisor: */
   flags = 0;
   if (flagexit)
      flags |= SUPER_EXITFLAG;
   if (sv->haslog)
      flags |= SUPER_HASLOG;
   sv->binstat[16] = flags;

   for (which = SUBSV_MAIN; which <= SUBSV_LOG; ++which) {
      subsv_t       *subsv = &sv->subsv[which];
      size_t         offset;

      if ((which == SUBSV_LOG) && !sv->haslog)
         break;

      offset = (which == SUBSV_MAIN) ? 34 : 52;
      flags = 0;

      /* SUBSV_UP, SUBSV_RESET, SUBSV_PAUSE: */
      if (subsv->pid != 0) {
         flags |= SUBSV_UP;
         if (subsv->isreset)
            flags |= SUBSV_RESET;
         if (subsv->ispaused)
            flags |= SUBSV_PAUSE;
      }

      /* SUBSV_WANT: */
      if (subsv->pid != 0) {
         if ((subsv->isreset && !subsv->wantdown) ||
             (subsv->wantdown && !subsv->isreset))
            flags |= SUBSV_WANT;
      } else {
         if (!subsv->wantdown)
            /* pid = 0, but do not want to be down: */
            flags |= SUBSV_WANT;
      }

      /* SUBSV_ONCE: */
      if (subsv->isonce)
         flags |= SUBSV_ONCE;

      sv->binstat[offset] = flags;
   }

   ++statchange;
   return;
}


/* binstat_pidchange()
**   update binary-encoded status with new pid for subservice which
*/
static
void binstat_pidchange(struct super *sv, subsv_id which)
{
   subsv_t       *subsv = &sv->subsv[which];
   size_t         offset = (which == SUBSV_MAIN) ? 18 : 36;

   upak32_pack(&sv->binstat[offset], subsv->pid);
   offset += 4;
   tain_pack(&sv->binstat[offset], &subsv->when);

   ++statchange;
   return;
}


/* binstat_post():
**   write binary-encoded status to control directory
**   note: operates relative to base directory
*/
static
void binstat_post(struct super *sv)
{
   int            fd;
   ssize_t        w;

   /* update flags before posting: */
   binstat_setflags(sv);

   /* TODO
    ** optionally disable the following block if serving status
    ** only through the control interface
    */
   if ((fd = open(binstat_tmp, (O_WRONLY | O_CREAT | O_TRUNC), 0644)) == -1) {
      warn_syserr("failure open() for ", STATUS_TMP,
                  " in service control directory");
      goto fail;
   }

   do {
      w = write(fd, sv->binstat, sizeof(binstat_t));
   } while ((w == -1) && (errno == EINTR));
   if (w == -1) {
      warn_syserr("failure write() on ", STATUS_TMP,
                  " in service control directory");
      close(fd);
      goto fail;
   }
   close(fd);

   if (w < sizeof(binstat_t)) {
      warn_syserr("incomplete write() on ", STATUS_TMP,
                  " in service control directory");
      goto fail;
   }
   if (rename(binstat_tmp, binstat_bin) == -1) {
      warn_syserr("failure rename() on ", STATUS_TMP, " to ", STATUS_BIN,
                  " in service control directory");
      goto fail;
   }

   /* status file now current: */
   statchange = 0;

   /* continuing here on failure */
 fail:

   return;
}


/* subsv_exec():
**   for subsv identified by which (main or log)
**   fork/exec runscript with target what (start or reset)
*/
static
void subsv_exec(struct super *sv, subsv_id which, enum runwhat_t what)
{
   subsv_t       *subsv = &sv->subsv[which];
   char          *prog[7];
   tain_t         now;
   tain_t         when_ok;
   tain_t         wait = tain_INIT(0, 0);
   pid_t          pid;

   /* insanity checks: */
   if ((sv->haslog == 0) && (which == SUBSV_LOG)) {
      /* logging service not enabled: */
      return;
   }
   if (subsv->pid != 0) {
      /* service still running: */
      return;
   }

   /* setup argv: */
   prog[0] = ((which == SUBSV_LOG) ? "./rc.log" : "./rc.main");
   prog[1] = ((what == RUN_START) ? "start" : "reset");
   prog[2] = (char *) svdir;
   prog[3] = NULL;

   /* arguments if running reset: */
   if (what == RUN_RESET) {
      int            wstat = subsv->wstat;

      if (WIFEXITED(wstat)) {
         char           nstr[NFMT_SIZE];
         prog[3] = "exit";
         prog[4] = nfmt_uint32(nstr, (uint32_t) WEXITSTATUS(wstat));
         prog[5] = NULL;
      } else {
         int            n =
             (WIFSIGNALED(wstat) ? WTERMSIG(wstat) : WSTOPSIG(wstat));
         char           nstr[NFMT_SIZE];
         char          *s = (char *) sysstr_signal(n);
         prog[3] = (WIFSIGNALED(wstat) ? "signal" : "stopped");
         prog[4] = nfmt_uint32(nstr, (uint32_t) n);
         prog[5] = ((s != NULL) ? s : "SIGUNKNOWN");
         prog[6] = NULL;
      }
   }

   /* timestamps and respawn governor: */
   tain_now(&now);
   tain_assign(&when_ok, &subsv->when_ok);
   if ((what == RUN_START) && tain_less(&now, &when_ok)) {
      log_warning("setting respawn governor on 'start' target of ", prog[0]);
      tain_minus(&wait, &when_ok, &now);
   }

   /* fork/exec: */
   while ((pid = fork()) == -1) {
      warn_syserr("failure fork() for starting child process");
      log_warning("wedging for retry in 9 seconds...");
      sleep(9);
   }
   /* child: */
   if (pid == 0) {
      /* cwd for runscripts is svdir: */
      if (fchdir(sv->fdsvdir) == -1) {
         fatal_syserr("(in child) failure fchdir() to service directory");
      }
      /* setup logpipe: */
      if (sv->haslog) {
         if (which == SUBSV_MAIN) {
            /* set stdout to logpipe: */
            close(1);
            if (dup2(sv->logpipe[1], 1) != 1) {
               fatal_syserr("(in child) ",
                            "failure dup2() on logpipe[1] to logging service");
            }
         }
         if ((which == SUBSV_LOG) && (what == RUN_START)) {
            /* set stdin to logpipe:
             **   (but not if this is a resetting log service)
             */
            close(0);
            if (dup2(sv->logpipe[0], 0) != 0) {
               fatal_syserr("(in child) ",
                            "failure dup2() on logpipe[0] for logging service");
            }
         }
         close(sv->logpipe[0]);
         close(sv->logpipe[1]);
      }
      /* clear signal handlers from child process: */
      sig_uncatch(SIGTERM);
      sig_uncatch(SIGCHLD);
      sig_uncatch(SIGPIPE);
      sigset_unblock(&sv->sigset);
      /* respawn governor: */
      if ((what == RUN_START) && !(tain_iszero(&wait))) {
         tain_pause(&wait, NULL);
      }
      /* go forth my child: */
      execve(prog[0], prog, environ);
      /* nuts, exec failed: */
      fatal_syserr("(in child) failure execve()");
   }

   /* parent: */
   subsv->pid = pid;
   subsv->isreset = ((what == RUN_RESET) ? 1 : 0);
   subsv->wstat = 0;
   /* timestamps and respawn governor: */
   tain_assign(&subsv->when, &now);
   if (what == RUN_START) {
      /* when_ok = now + 1sec + wait: */
      tain_LOAD(&when_ok, 1, 0);
      tain_plus(&when_ok, &now, &when_ok);
      tain_plus(&when_ok, &when_ok, &wait);
      tain_assign(&subsv->when_ok, &when_ok);
   }
   /* update status: */
   binstat_pidchange(sv, which);

   return;
}


/* do_kill():
*/
static
void do_kill(struct super *sv, subsv_id which, int sig)
{
   /* deliver signal if running start: */
   if (!sv->subsv[which].isreset) {
      kill(sv->subsv[which].pid, sig);
      return;
   }

   /* filter signal if running reset: */
   switch (sig) {
   case (SIGCONT):
   case (SIGKILL):
      log_warning("sending ", sysstr_signal(sig), " to resetting service");
      kill(sv->subsv[which].pid, sig);
      break;
   default:
      log_warning("dropping ", sysstr_signal(sig), " to resetting service");
   }
   return;
}


/* do_control():
**   process the control command in cmd for subservice which
**   return:
**     0 : no error
**    -1 : command not recognized
*/
static
int do_control(struct super *sv, subsv_id which, uchar_t cmd)
{
   subsv_t       *subsv = &sv->subsv[which];
   pid_t          pid;

   pid = subsv->pid;

   /* dispatch command: */
   switch (cmd) {
   case 'X':
      /* meta-command: "exit" stop supervisor */
      /* note: meta-commands ingored for log subservice: */
      if (which == SUBSV_LOG)
         break;
      /* setup for exit: */
      flagexit = 1;
      break;
   case 'D':
      /* meta-command: "down" both main and log: */
      /* note: meta-commands ingored for log subservice: */
      if (which == SUBSV_LOG)
         break;
      do_control(sv, SUBSV_MAIN, 'd');
      do_control(sv, SUBSV_LOG, 'd');
      break;
   case 'U':
      /* meta-command: "up" both main and log: */
      /* note: meta-commands ingored for log subservice: */
      if (which == SUBSV_LOG)
         break;
      do_control(sv, SUBSV_LOG, 'u');
      do_control(sv, SUBSV_MAIN, 'u');
      break;
   case 'd':
      /* faux signal: "down" */
      subsv->wantdown = 1;
      if (pid > 0) {
         do_control(sv, which, 't');
         do_control(sv, which, 'c');
      }
      ++statchange;
      break;
   case 'u':
      /* faux signal: "up" */
      subsv->isonce = 0;
      subsv->wantdown = 0;
      if (pid == 0) {
         subsv_exec(sv, which, RUN_START);
      } else {
         ++statchange;
      }
      break;
   case 'o':
      /* faux signal: "once" */
      subsv->isonce = 1;
      subsv->wantdown = 0;
      if (pid == 0) {
         subsv_exec(sv, which, RUN_START);
      } else {
         ++statchange;
      }
      break;
      /* true signals: */
   case 'a':
      /* "alarm": */
      if (pid > 0) {
         do_kill(sv, which, SIGALRM);
      }
      break;
   case 'c':
      /* "continue": */
      subsv->ispaused = 0;
      if (pid > 0) {
         do_kill(sv, which, SIGCONT);
      }
      ++statchange;
      break;
   case 'h':
      /* "hangup": */
      if (pid > 0) {
         do_kill(sv, which, SIGHUP);
      }
      break;
   case 'i':
      /* "interrupt": */
      if (pid > 0) {
         do_kill(sv, which, SIGINT);
      }
      break;
   case 'k':
      /* "kill": */
      if (pid > 0) {
         do_kill(sv, which, SIGKILL);
      }
      break;
   case 'p':
      /* "pause": */
      /* do not set "ispaused" flag if service is resetting: */
      if ((pid > 0) && (!subsv->isreset)) {
         do_kill(sv, which, SIGSTOP);
         subsv->ispaused = 1;
         ++statchange;
      }
      break;
   case 'q':
      /* "quit": */
      if (pid > 0) {
         do_kill(sv, which, SIGQUIT);
      }
      break;
   case 't':
      /* "terminate": */
      if (pid > 0) {
         do_kill(sv, which, SIGTERM);
      }
      break;
   case 'w':
#ifdef SIGWINCH
      /* "sigwinch" (recognized by mathopd): */
      if (pid > 0) {
         do_kill(sv, which, SIGWINCH);
      }
#endif
      break;
   case '1':
      /* "usr1": */
      if (pid > 0) {
         do_kill(sv, which, SIGUSR1);
      }
      break;
   case '2':
      /* "usr2": */
      if (pid > 0) {
         do_kill(sv, which, SIGUSR2);
      }
      break;
   default:
      /* unknown command (a protocol error): */
      return -1;
      break;
   }

   /* success: */
   return 0;
}


/* proto_error():
**   reply to client connection with 'E' packet
**   terminate connection
**   err may be:
**     0  : request ok
**     >0 : some error handling request
**
**   XXX, assumes errno on platform always > 0
*/
static
void proto_error(int fd, int err)
{
   pkt_t          reply = pkt_INIT(1, 'E', 4);
   uchar_t       *payload = pkt_data(reply);

   /* XXX: clients possibly buggered if errno negative */
   if (err < 0) {
      log_warning
          ("buggerall: negative errno cast to unsigned in proto_error() reply");
   }
   /* log error reply: */
   if (err > 0) {
      log_warning
          ("sending non-zero proto_error() reply to client on control socket");
   }
   upak32_pack(payload, (uint32_t) err);

   /* don't care about error from pkt_write() ... */
   pkt_write(fd, reply);

   return;
}


/* proto_status():
**   service status request from client connection
**   reply 'S' packet
**   terminate connection
*/
static
void proto_status(struct super *sv, int fd)
{
   pkt_t          reply;

   pkt_load(reply, 1, 'S', sv->binstat, sizeof(binstat_t));

   /* reply; don't care about error from pkt_write(): */
   pkt_write(fd, reply);

   return;
}


/* check_control():
**   read pkt on input control socket
**   dispatch command read to the do_control() function
*/
static
void check_control(struct super *sv)
{
   int            fifo_0, fifo_1;
   pkt_t          pkt;
   uchar_t       *payload;
   subsv_id       which = SUBSV_MAIN;
   int            e;

   fifo_0 = sv->fdfifo_0;
   fifo_1 = sv->fdfifo_1;

   /* read control request packet from input fifo: */
   e = pkt_read(fifo_0, pkt);
   if (e == -1) {
      int            terrno = errno;
      warn_syserr("pkt_read() error on control fifo");
      proto_error(fifo_1, terrno);
      return;
   }
   if (pkt_proto(pkt) != 1) {
      log_warning("protocol mismatch in pkt_read() from control fifo");
      proto_error(fifo_1, EPROTO);
      return;
   }

   /* dispatch according to packet type: */
   switch (pkt_type(pkt)) {
   case 'C':                   /* service command: */
      /* just one command per packet please: */
      if (pkt_size(pkt) != 1) {
         proto_error(fifo_1, EPROTO);
      }
      payload = pkt_data(pkt);
      /* unshift command destined for log: */
      if (payload[0] > 0x7f) {
         payload[0] -= 0x7f;
         which = SUBSV_LOG;
      }
#ifndef NDEBUG
      {
         char           cs[2] = { payload[0], '\0' };
         char          *ws = (which == SUBSV_MAIN ? "main" : "log");
         log_debug("processing control command `", cs, "' for ",
                   ws, " service");
      }
#endif
      e = do_control(sv, which, payload[0]);
      proto_error(fifo_1, (e == -1) ? EPROTO : 0);
      break;
   case 'Q':                   /* status query: */
      proto_status(sv, fifo_1);
      break;
   default:
      /* unknown pkt type: */
      proto_error(fifo_1, EPROTO);
      break;
   }

   /* control connection serviced: */
   return;
}


/* check_children():
**   find any exited subsv processes
**   update subsv binstat records
**   reset/restart as necessary
*/
static
void check_children(struct super *sv)
{
   pid_t          pid;
   int            wstat;
   subsv_id       which;
   int            exited[2] = { 0, 0 };

   /* flag terminated children: */
   while ((pid = waitpid(-1, &wstat, WNOHANG)) > 0) {
      if (pid == sv->subsv[SUBSV_MAIN].pid) {
         which = SUBSV_MAIN;
      } else if (pid == sv->subsv[SUBSV_LOG].pid) {
         which = SUBSV_LOG;
      } else {
         /* XXX, not my child */
         log_trace("not my child!");
         continue;
      }

      exited[which] = 1;

      {                         /* debug: */
         char          *subsv_name = ((which == SUBSV_MAIN) ? "main" : "log");
         char           nbuf[NFMT_SIZE];

         nfmt_uint32(nbuf, wstat);
         if (sv->subsv[which].isreset == 1) {
            log_debug(subsv_name, " service exited from reset! (", nbuf, ")");
         } else {
            log_debug(subsv_name, " service exited from start! (", nbuf, ")");
         }

         if (WIFEXITED(wstat)) {
            nfmt_uint32(nbuf, WEXITSTATUS(wstat));
            log_debug(subsv_name, " WIFEXITED() true with exit status ",
                      nbuf);
         }
         if (WIFSIGNALED(wstat)) {
            nfmt_uint32(nbuf, WTERMSIG(wstat));
            log_debug(subsv_name, " WIFSIGNALED() true with signal ", nbuf,
                      " \"", sysstr_signal(WTERMSIG(wstat)), "\"");
         }
      }

      sv->subsv[which].pid = 0;
      sv->subsv[which].wstat = wstat;
      if (sv->subsv[which].isonce == 1) {
         sv->subsv[which].wantdown = 1;
      }
      binstat_pidchange(sv, which);
   }

   /* check for reset/restart: */
   for (which = 0; which < 2; ++which) {
      subsv_t       *subsv = &sv->subsv[which];

      if ((subsv->pid != 0) || ((which == SUBSV_LOG) && (sv->haslog == 0))) {
         continue;
      }

      /* pid = 0: */
      if (exited[which] == 1) {

         /* exited from start? --> reset: */
         if (subsv->isreset != 1) {
            subsv_exec(sv, which, RUN_RESET);
            continue;
         }

         /* exited from reset? --> start: */
         if (subsv->wantdown == 0) {
            subsv_exec(sv, which, RUN_START);
         }
      }
   }

   return;
}


/* shut_down():
**   begin shut_down sequence
**   return:
**     0: shut_down incomplete
**     1: shut_down complete
*/
static
int shut_down(struct super *sv)
{
   subsv_t       *subsv;

   log_debug("in shut_down()");

   /* first bring down main service: */
   subsv = &sv->subsv[SUBSV_MAIN];
   if (subsv->pid != 0) {
      /* something is running: */
      if (subsv->isreset) {
         /* currently running reset: make sure wantdown is set and not paused: */
         subsv->wantdown = 1;
         do_control(sv, SUBSV_MAIN, 'c');
      } else {
         /* currently running start: put it down: */
         do_control(sv, SUBSV_MAIN, 'd');
      }
      return 0;
   }

   /* note:
    **   in shut_down(), main pid can be 0 only when
    **     + already down (nothing to do)
    **     + pending new start
    **
    **   cannot otherwise be in shut_down() with pid 0:
    **     (eg, after start exits, before reset runs)
    **     because check_children() runs reset immediately 
    */

   /* main is currently down, but possibly pending new start: */
   if ((subsv->pid == 0) && (subsv->wantdown != 0)) {
      /* fixup flags so it stays down: */
      subsv->wantdown = 1;
      ++statchange;
      /* proceed to shut_down log service ... */
   }

   /* main is down, no log service to deal with, all done: */
   if (sv->haslog == 0) {
      return 1;
   }

   /* main is down, bring down log service: */
   subsv = &sv->subsv[SUBSV_LOG];
   if (subsv->pid != 0) {
      /* something is running: */
      if (subsv->isreset) {
         /* currently running reset: make sure wantdown is set and not paused: */
         subsv->wantdown = 1;
         do_control(sv, SUBSV_LOG, 'c');
      } else {
         /* currently running start: close logpipe and put it down: */
         log_debug("closing logpipe[1] on logging service ...");
         close(sv->logpipe[1]);
         do_control(sv, SUBSV_LOG, 'd');
      }
      return 0;
   }

   /* note:
    **   log pid can be 0 in shut_down() only as described for main pid above
    **     + already down (nothing to do)
    **     + pending new start
    **
    **   cannot otherwise be in shut_down() with pid 0:
    **     (eg, after start exits, before reset runs)
    **     because check_children() runs reset immediately 
    */

   /* log is currently down, but possibly pending new start: */
   if ((subsv->pid == 0) && (subsv->wantdown != 0)) {
      subsv->wantdown = 1;
      ++statchange;
      /* proceed to shut_down completion... */
   }

   /* main is down, log is down, all done: */
   return 1;
}


/* main_loop():
**   poll() for input on selfpipe and control socket 
**   process
*/
static
void main_loop(struct super *sv)
{
   struct pollfd  pfd[2];
   int            e;

   pfd[0].fd = selfpipe[0];
   pfd[0].events = POLLIN;
   pfd[1].fd = sv->fdfifo_0;
   pfd[1].events = POLLIN;

   for (;;) {
      /* all done? */
      if ((flagexit) && (shut_down(sv))) {
         break;
      }

      /* write updated status: */
      if (statchange) {
         binstat_post(sv);
      }

      /* poll while catching signals: */
      sigset_unblock(&sv->sigset);
      {
         log_debug("calling poll() ...");
         do {
            e = poll(pfd, 2, -1);
         } while ((e == -1) && (errno == EINTR));

      }
      sigset_block(&sv->sigset);

      if (e == -1) {
         warn_syserr("failure poll() in main_loop()");
         continue;
      }

      /* signals? */
      if ((pfd[0].revents & POLLIN)) {
         char           c;

         /* consume any signal triggers: */
         while ((read(selfpipe[0], &c, 1)) == 1) {
            /* empty */ ;
         }

         /* handle terminated children: */
         check_children(sv);
      }

      /* received control input? */
      if (pfd[1].revents & POLLIN) {
         check_control(sv);
      }
   }

   return;
}


int main(int argc, char *argv[])
{
   static char    pidbuf[NFMT_SIZE];
   char           opt;
   nextopt_t      nopt = nextopt_INIT(argc, argv, ":hVo");
   struct super   sv;
   int            opt_once = 0;

   /* svdir until args processed: */
   svdir = "-";
   /* pid for pidlock and logging: */
   mypid = getpid();
   progpid = nfmt_uint32(pidbuf, (uint32_t) mypid);
   /* progname for logging: */
   progname = nextopt_progname(&nopt);
   while ((opt = nextopt(&nopt))) {
      char           optc[2] = { nopt.opt_got, '\0' };
      switch (opt) {
      case 'h':
         usage();
         die(0);
         break;
      case 'V':
         version();
         die(0);
         break;
      case 'o':
         opt_once = 1;
         break;
      case '?':
         if (nopt.opt_got != '?') {
            fatal_usage("invalid option: -", optc);
         }
         /* else fallthrough: */
      default:
         die_usage();
         break;
      }
   }
   argc -= nopt.arg_ndx;
   argv += nopt.arg_ndx;

   /* svdir is global for descriptive stderr reporting: */
   svdir = argv[0];
   if ((svdir == NULL) || (svdir[0] == '\0')) {
      fatal_usage("missing service directory argument");
   }

   log_info("starting ...");

   super_init(&sv);
   if (opt_once) {
      sv.flagonce = 1;
      log_debug("option -o: setting flagonce");
   }

   /* initialize signal set and block: */
   sigset_add(&sv.sigset, SIGTERM);
   sigset_add(&sv.sigset, SIGCHLD);
   sigset_block(&sv.sigset);

   /* initialize signal handlers: */
   sig_catch(SIGTERM, &sig_handler);
   sig_catch(SIGCHLD, &sig_handler);

   /*
    ** essential to catch/ignore SIGPIPE for non-blocking use of socket
    ** BUT: be sure to restore the default handler for child processes!
    */
   sig_ignore(SIGPIPE);

   /* initialize selfpipe, control directory and service directory: */
   setup_selfpipe();
   setup_control(&sv);
   setup_service(&sv);

   /*
    ** no fatals beyond this point
    */

   /* initial startup of service: */
   service_boot(&sv);

   /* monitor: */
   main_loop(&sv);

   /* XXX, any cleanup? */
   log_info("terminating normally");
   return 0;
}

/* eof: perpetrate.c */