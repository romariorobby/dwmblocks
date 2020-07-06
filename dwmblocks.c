#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>

#define CMDLENGTH			25
#define STTLENGTH			256
#define LOCKFILE			"/tmp/dwmblocks.pid"

#define EMPTYCMDOUT(block)		(block->cmdoutcur[0] == '\n' || block->cmdoutcur[0] == '\0')
#define NOTATCMDOUTEND(block, i)	(i < CMDLENGTH && block->cmdoutcur[i] != '\n' && block->cmdoutcur[i] != '\0')

typedef struct {
	char *pathu;
	char *pathc;
	const int interval;
	const int signal;
        char cmdoutcur[CMDLENGTH];
        char cmdoutprv[CMDLENGTH];
} Block;

static void buttonhandler(int signal, siginfo_t *si, void *ucontext);
static void getcmd(Block *block, int *sigval);
static void setroot();
static void setupsignals();
static void sighandler(int signal, siginfo_t *si, void *ucontext);
static void statusloop();
static void termhandler(int signum);
static int updatestatus();
static void writepid();

#include "blocks.h"

static int statusContinue = 1;
static char statusstr[STTLENGTH];
static size_t delimlength;
static Display *dpy;

void
buttonhandler(int signal, siginfo_t *si, void *ucontext)
{
	signal = si->si_value.sival_int >> 8;
	switch (fork()) {
                case -1:
                        perror("buttonhandler - fork");
                        exit(1);
                case 0:
                        close(ConnectionNumber(dpy));
                        for (Block *current = blocks; current->pathu; current++) {
                                if (current->signal == signal) {
                                        char button[] = { '0' + (si->si_value.sival_int & 0xff), '\0' };
                                        char *arg[] = { current->pathc, button, NULL };

                                        setsid();
                                        execv(arg[0], arg);
                                        perror("buttonhandler - execv");
                                        _exit(127);
                                }
                        }
                        exit(0);
        }
}

void
getcmd(Block *block, int *sigval)
{
        int fd[2];

        if (pipe(fd) == -1) {
                perror("getcmd - pipe");
                exit(1);
        }
        switch (fork()) {
                case -1:
                        perror("getcmd - fork");
                        exit(1);
                case 0:
                        close(ConnectionNumber(dpy));
                        close(fd[0]);
                        if (dup2(fd[1], STDOUT_FILENO) != STDOUT_FILENO) {
                                perror("getcmd - dup2");
                                exit(1);
                        }
                        close(fd[1]);
                        if (sigval) {
                                char buf[12];
                                char *arg[] = { block->pathu, buf, NULL };

                                snprintf(buf, sizeof buf, "%d", *sigval);
                                execv(arg[0], arg);
                        } else {
                                char *arg[] = { block->pathu, NULL };

                                execv(arg[0], arg);
                        }
                        perror("getcmd - execv");
                        _exit(127);
                default:
                        close(fd[1]);
                        if (read(fd[0], block->cmdoutcur, CMDLENGTH) == -1) {
                                perror("getcmd - read");
                                exit(1);
                        }
                        close(fd[0]);
        }
}

void
setroot()
{
	if (updatestatus()) /* only set root if block outputs have changed */
		return;
	XStoreName(dpy, DefaultRootWindow(dpy), statusstr);
        XFlush(dpy);
}

void
setupsignals()
{
        struct sigaction sa;

        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        /* to handle INT, HUP and TERM */
        sa.sa_handler = termhandler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
        /* to ignore unused realtime signals */
        sa.sa_handler = SIG_IGN;
        for (int i = SIGRTMIN; i <= SIGRTMAX; i++)
                sigaction(i, &sa, NULL);
        /* to handle signals generated by dwm on click events */
	sa.sa_flags = SA_RESTART | SA_SIGINFO;
	sa.sa_sigaction = buttonhandler;
	sigaction(SIGRTMIN, &sa, NULL);
        /* to handle update signals for individual blocks */
        sa.sa_sigaction = sighandler;
	for (Block *current = blocks; current->pathu; current++)
		if (current->signal > 0)
			sigaction(SIGRTMIN + current->signal, &sa, NULL);
        /* to prevent forked children from becoming zombies */
	sa.sa_flags = SA_NOCLDWAIT | SA_RESTART;
	sa.sa_handler = SIG_DFL;
        sigaction(SIGCHLD, &sa, NULL);
}

void
sighandler(int signal, siginfo_t *si, void *ucontext)
{
	signal -= SIGRTMIN;
	for (Block *current = blocks; current->pathu; current++) {
		if (current->signal == signal)
			getcmd(current, &(si->si_value.sival_int));
	}
	setroot();
}

void
statusloop()
{
	int i;

	setupsignals();
        for (Block *current = blocks; current->pathu; current++)
                if (current->interval >= 0)
                        getcmd(current, NULL);
        setroot();
        sleep(SLEEPINTERVAL);
        i = SLEEPINTERVAL;
	while (statusContinue) {
                for (Block *current = blocks; current->pathu; current++)
                        if (current->interval > 0 && i % current->interval == 0)
                                getcmd(current, NULL);
		setroot();
		sleep(SLEEPINTERVAL);
		i += SLEEPINTERVAL;
	}
}

void
termhandler(int signum)
{
	statusContinue = 0;
}

/* returns whether block outputs have changed and updates statusstr if they have */
int
updatestatus()
{
        int i;
        char *str = statusstr;
        Block *current;

        for (current = blocks; current->pathu; current++) {
                if (EMPTYCMDOUT(current)) {
                        if (current->cmdoutprv[0] != current->cmdoutcur[0])
                                goto updatestrstrt;
                        continue;
                }
                i = 0;
                do {
                        if (current->cmdoutcur[i] == current->cmdoutprv[i]) {
                                i++;
                                continue;
                        } else {
                                str += i;
                                goto updatestrmddl;
                        }
                } while (NOTATCMDOUTEND(current, i));
                str += i;
                if (current->pathc && current->signal)
                        str++;
                str += delimlength;
        }
	return 1;
updatestrstrt:
        current->cmdoutprv[0] = current->cmdoutcur[0];
        for (current++; current->pathu; current++) {
                if (EMPTYCMDOUT(current)) {
                        current->cmdoutprv[0] = current->cmdoutcur[0];
                        continue;
                }
                i = 0;
updatestrmddl:
                do {
                        *(str++) = current->cmdoutcur[i];
                        current->cmdoutprv[i] = current->cmdoutcur[i];
                        i++;
                } while (NOTATCMDOUTEND(current, i));
                if (current->pathc && current->signal)
                        *(str++) = current->signal;
                for (i = 0; delim[i]; i++)
                        *(str++) = delim[i];
                *(str++) = '\n';
        }
        if (str != statusstr)
                *(str - delimlength) = '\0';
        return 0;
}

void
writepid()
{
        int fd;
        char buf[20];
        struct flock fl;
        ssize_t len;

        fd = open(LOCKFILE, O_RDWR|O_CREAT, 0644);
        if (fd == -1) {
                perror("writepid - fd");
                exit(1);
        }
        fl.l_type = F_WRLCK;
        fl.l_start = 0;
        fl.l_whence = SEEK_SET;
        fl.l_len = 0;
        if (fcntl(fd, F_SETLK, &fl) == -1) {
                if (errno == EACCES || errno == EAGAIN) {
                        fputs("Error: another instance of dwmblocks is already running.\n", stderr);
                        exit(2);
                }
                perror("lockfile - fcntl");
                exit(1);
        }
        if (ftruncate(fd, 0) == -1) {
                perror("writepid - ftruncate");
                exit(1);
        }
        snprintf(buf, sizeof buf, "%ld", (long)getpid());
        len = strlen(buf);
        if (write(fd, buf, len) != len) {
                perror("writepid - write");
                exit(1);
        }
}

int
main(int argc, char *argv[])
{
        writepid();
        if (argc > 2) {
                if (strcmp("-d", argv[1]) == 0)
                        delim = argv[2];
        }
        delimlength = strlen(delim) + 1;
        if (!(dpy = XOpenDisplay(NULL))) {
                fputs("Error: could not open display.\n", stderr);
                return 1;
        }
	statusloop();
        unlink(LOCKFILE);
        XStoreName(dpy, DefaultRootWindow(dpy), "");
        XCloseDisplay(dpy);
        return 0;
}
