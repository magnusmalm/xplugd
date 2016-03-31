/* xplugd - monitor plug/unplug helper
 *
 * Copyright (C) 2012-2015  Stefan Bolte <portix@gmx.net>
 * Copyright (C) 2016       Joachim Nilsson <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#define SYSLOG_NAMES
#include <syslog.h>
#include <unistd.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#define OCNE(X) ((XRROutputChangeNotifyEvent*)X)
#define MSG_LEN 128

static int   loglevel = LOG_NOTICE;
static char *con_actions[] = { "connected", "disconnected", "unknown" };
extern char *__progname;

static int loglvl(char *level)
{
	for (int i = 0; prioritynames[i].c_name; i++) {
		if (!strcmp(prioritynames[i].c_name, level))
			return prioritynames[i].c_val;
	}

	return atoi(level);
}

static int error_handler(void)
{
	exit(1);
}

static void catch_child(int sig)
{
	(void)sig;
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

static void handle_event(Display *dpy, XRROutputChangeNotifyEvent *ev, char *cmd)
{
	char msg[MSG_LEN];
	static char old_msg[MSG_LEN] = "";
	XRROutputInfo *info;
	XRRScreenResources *resources;

	resources = XRRGetScreenResources(ev->display, ev->window);
	if (!resources) {
		syslog(LOG_ERR, "Could not get screen resources");
		return;
	}

	info = XRRGetOutputInfo(ev->display, resources, ev->output);
	if (!info) {
		syslog(LOG_ERR, "Could not get output info");
		XRRFreeScreenResources(resources);
		return;
	}

	/* Check for duplicate plug events */
	snprintf(msg, sizeof(msg), "%s %s", info->name, con_actions[info->connection]);
	if (!strcmp(msg, old_msg)) {
		if (loglevel == LOG_DEBUG)
			syslog(LOG_DEBUG, "Same message as last time, time %lu, skipping ...", info->timestamp);
		goto done;
	}
	strcpy(old_msg, msg);

	if (loglevel == LOG_DEBUG) {
		syslog(LOG_DEBUG, "Event: %s %s", info->name, con_actions[info->connection]);
		syslog(LOG_DEBUG, "Time: %lu", info->timestamp);
		if (info->crtc == 0) {
			syslog(LOG_DEBUG, "Size: %lumm x %lumm", info->mm_width, info->mm_height);
		} else {
			XRRCrtcInfo *crtc;

			syslog(LOG_DEBUG, "CRTC: %lu", info->crtc);

			crtc = XRRGetCrtcInfo(dpy, resources, info->crtc);
			if (crtc) {
				syslog(LOG_DEBUG, "Size: %dx%d", crtc->width, crtc->height);
				XRRFreeCrtcInfo(crtc);
			}
		}
	}

	syslog(LOG_DEBUG, "Calling %s ...", cmd);
	if (!fork()) {
		char *args[] = {
			cmd,
			"display",
			info->name,
			con_actions[info->connection],
			NULL,
		};

		setsid();
		if (dpy)
			close(ConnectionNumber(dpy));

		execv(args[0], args);
		syslog(LOG_ERR, "Failed calling %s: %s", cmd, strerror(errno));
		exit(0);
	}

done:
	XRRFreeOutputInfo(info);
	XRRFreeScreenResources(resources);
}

static int usage(int status)
{
	printf("Usage: %s [OPTIONS] script\n\n"
	       "Options:\n"
	       "  -h        Print this help text and exit\n"
	       "  -l LEVEL  Set log level: none, err, info, notice*, debug\n"
	       "  -n        Run in foreground, do not fork to background\n"
	       "  -s        Use syslog, even if running in foreground, default w/o -n\n"
	       "  -v        Show program version\n\n"
	       "Copyright (C) 2012-2015 Stefan Bolte\n"
	       "Copyright (C)      2016 Joachim Nilsson\n\n"
	       "Bug report address: https://github.com/troglobit/xplugd/issues\n\n", __progname);
	return status;
}

static int version(void)
{
	printf("v%s\n", VERSION);
	return 0;
}

int main(int argc, char *argv[])
{
	int c, log_opts = LOG_CONS | LOG_PID;
	XEvent ev;
	Display *dpy;
	int background = 1, logcons = 0;
	uid_t uid;

	while ((c = getopt(argc, argv, "hl:nsv")) != EOF) {
		switch (c) {
		case 'h':
			return usage(0);

		case 'l':
			loglevel = loglvl(optarg);
			break;

		case 'n':
			background = 0;
			logcons++;
			break;

		case 's':
			logcons--;
			break;

		case 'v':
			return version();

		default:
			return usage(1);
		}
	}

	if (optind >= argc)
		return usage(1);

	if (((uid = getuid()) == 0) || uid != geteuid()) {
		fprintf(stderr, "%s may not run as root\n", __progname);
		exit(1);
	}

	if ((dpy = XOpenDisplay(NULL)) == NULL) {
		fprintf(stderr, "Cannot open display\n");
		exit(1);
	}

	if (background)
		daemon(0, 0);
	if (logcons > 0)
		log_opts |= LOG_PERROR;

	openlog(NULL, log_opts, LOG_USER);
	setlogmask(LOG_UPTO(loglevel));

	signal(SIGCHLD, catch_child);

	XRRSelectInput(dpy, DefaultRootWindow(dpy), RROutputChangeNotifyMask);
	XSync(dpy, False);
	XSetIOErrorHandler((XIOErrorHandler)error_handler);

	while (1) {
		if (!XNextEvent(dpy, &ev))
			handle_event(dpy, OCNE(&ev), argv[optind]);
	}

	return 0;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
