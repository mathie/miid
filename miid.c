/* 
   miid - Monitor the link state of Ethernet devices and attempt to bring
   up or down the interface when the link state changes.

   Copyright (C) 2002 Graeme Mathieson

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  

*/

#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

#define VERSION "0.1"

#define DEFAULT_DEV "eth0"
#define DEFAULT_IFUP "/sbin/ifup"
#define DEFAULT_IFDOWN "/sbin/ifdown"
#define DEFAULT_DELAY 5

#define NEW_IOCTL_NUM 0x8947
#define OLD_IOCTL_NUM SIOCDEVPRIVATE

#define LINK_STATE 0x0004

#define syslog_die(str) do { \
  syslog (LOG_ERR, "%s failed %s, exiting...", str, strerror(errno)); \
  exit (EXIT_FAILURE); \
} while (0)

typedef enum { DOWN, UP } state;

static void usage (int status);

/* The name the program was run with, stripped of any leading path. */
char *program_name;

/* getopt_long return codes */
enum {DUMMY_CODE=129
};

/* Option flags and variables */
static int daemonize = 1;
static char *dev = NULL;
static char *ifup_cmd = NULL;
static char *ifdown_cmd = NULL;
static int delay = -1;

/* File-scope variables */
static int skfd = -1;
static int ioctl_num = NEW_IOCTL_NUM;
static unsigned int phy_id;
static struct ifreq ifr;

static int decode_switches (int argc, char **argv);
static int setup_signals (void);
static void sig_handler (int signo);
static void quit (void);
static int setup_mii (void);
static void monitor_mii (void);
static int mdio_read (int location);
static void ifdown(void);
static void ifup (void);

int
main (int argc, char **argv)
{
  int i;

  program_name = argv[0];

  /* Figure out command-line arguments */
  i = decode_switches (argc, argv);

  /* Turn into a daemon unless otherwise requested */
  if (daemonize) {
    if (daemon (0, 0) < 0) {
      perror("daemon()");
    }
  }

  /* Initialise syslog and announce our starting */
  openlog (basename (program_name), LOG_PID, LOG_DAEMON);
  syslog (LOG_INFO, "Starting");

  /* Setup sighandlers for the stuff I want to be able to catch */
  if (setup_signals() < 0) {
      syslog_die ("setup_signals()");
  }

  if (dev == NULL) {
    syslog (LOG_INFO, "Device not specified, using default of %s", DEFAULT_DEV);
    dev = DEFAULT_DEV;
  } else {
    syslog (LOG_INFO, "Monitoring device %s", dev);
  }

  if (ifup_cmd == NULL)
    ifup_cmd = DEFAULT_IFUP;
  if (ifdown_cmd == NULL)
    ifdown_cmd = DEFAULT_IFDOWN;
  if (delay == -1)
    delay = DEFAULT_DELAY;

  syslog (LOG_INFO, "link up script: %s, link down script: %s, "
      "delay: %d seconds.", ifup_cmd, ifdown_cmd, delay);

  /* Setup our access to the MII */
  if (setup_mii () < 0) {
    syslog_die ("setup_mii");
  }

  monitor_mii ();
  quit();			/* Never returns! */
  return 0; 			/* Keep gcc happy. */
}

static int
setup_mii ()
{
  u_int16_t *data = NULL;
  
  if ((skfd = socket (AF_INET, SOCK_DGRAM, 0)) < 0) {
    return -1;
  }
  memset (&ifr, sizeof (ifr), 0);
  strncpy (ifr.ifr_name, dev, IFNAMSIZ);
  
  /* I gather the ioctl num changed... */
  if (ioctl (skfd, NEW_IOCTL_NUM, &ifr) >= 0) {
    ioctl_num = NEW_IOCTL_NUM;
  } else if (ioctl (skfd, OLD_IOCTL_NUM, &ifr) >= 0) {
    ioctl_num = OLD_IOCTL_NUM;
  } else {
    syslog (LOG_ERR, "SIOCGMIIPHY on %s failed: %s", dev, strerror(errno));
    return -1;
  }
  data = (u_int16_t *) (&ifr.ifr_data);
  phy_id = data[0];

  syslog (LOG_DEBUG, "Using %s IOCTL number, phy_id %d.",
      (ioctl_num == NEW_IOCTL_NUM ? "new" : "old"), phy_id);
  return 0;
}

static void
monitor_mii ()
{
  unsigned short current, prev = mdio_read (1);
  int wait = 0;
  state direction = DOWN;
  state current_state = DOWN;
    
  if (prev == 0xffff) {
    syslog (LOG_ERR, "No MII transceiver present to monitor");
    return;
  }

  /* Do initial wotsits with the link state */
  if (prev & LINK_STATE) {
    /* Has link state on startup, bring up interface */
    ifup();
    current_state = UP;
  }
  
  for ( ; ; ) {
    current = mdio_read (1);
    if (current == 0xffff) {
      syslog (LOG_ERR, "MII transceiver is no longer accessible.");
      return;
    }
    if (prev != current) {
      if ((prev & LINK_STATE) && !(current & LINK_STATE)) {
	/* transition from UP -> DOWN */
	direction = DOWN;
	wait = delay;
      } else if (!(prev & LINK_STATE) && (current & LINK_STATE)) {
	/* transition from DOWN -> UP */
	direction = UP;
	wait = delay;
      }
    } else if (wait > 0) {
      wait--;
      if (wait == 0) {
	if ((direction == DOWN) && (current_state == UP)) {
	  ifdown ();
	  current_state = DOWN;
	} else if ((direction == UP) && (current_state == DOWN)) {
	  ifup ();
	  current_state = UP;
	}
      }
    }
    prev = current;
    sleep (1);
  }
}

static void
ifdown(void)
{
  int pid;
  syslog (LOG_INFO, "Link state lost.  Bringing down %s", dev);
  if ((pid = fork()) < 0) {
    syslog (LOG_ERR, "Failed to fork %s: %s", ifdown_cmd, strerror(errno));
    return;
  } else if (pid != 0) {
    /* Parent */
    return;
  } else {
    /* Child */
    if (execlp (ifdown_cmd, ifdown_cmd, dev, NULL) < 0) {
      syslog (LOG_ERR, "Failed to exec %s: %s", ifdown_cmd, strerror(errno));
      return;
    }
  }
}

static void
ifup (void)
{
  int pid;
  syslog (LOG_INFO, "Link state detected.  Bringing up %s", dev);
  if ((pid = fork()) < 0) {
    syslog (LOG_ERR, "Failed to fork %s: %s", ifup_cmd, strerror(errno));
    return;
  } else if (pid != 0) {
    /* Parent */
    return;
  } else {
    /* Child */
    if (execlp (ifup_cmd, ifup_cmd, dev, NULL) < 0) {
      syslog (LOG_ERR, "Failed to exec %s: %s", ifup_cmd, strerror(errno));
      return;
    }
  }
}

static int
mdio_read (int location)
{
  u_int16_t *data = (u_int16_t *) (&ifr.ifr_data);
  data[0] = phy_id;
  data[1] = location;

  if (ioctl (skfd, ioctl_num + 1, &ifr) < 0) {
    syslog (LOG_ERR, "SIOCGMIIREG on %s failed: %s\n", ifr.ifr_name,
	strerror (errno));
    return -1;
  }
  return data[3];
}

static void
quit ()
{
  syslog (LOG_INFO, "Terminating.");
  exit (0);
}

static int
setup_signals ()
{
  struct sigaction act;
  act.sa_handler = sig_handler;
  sigemptyset (&act.sa_mask);
  act.sa_flags = 0;
  if (sigaction (SIGTERM, &act, NULL) < 0) {
    return -1;
  }
  if (sigaction (SIGINT, &act, NULL) < 0) {
    return -1;
  }
  if (sigaction (SIGHUP, &act, NULL) < 0) {
    return -1;
  }
  return 0;
}

static void
sig_handler (int signo)
{
  if (signo == SIGINT) {
    syslog (LOG_INFO, "Received SIGINT.");
    quit();
  } else if (signo == SIGTERM) {
    syslog (LOG_INFO, "Received SIGTERM.");
    quit();
  } else if (signo == SIGHUP) {
    syslog (LOG_INFO, "Ooh, that tickles!");
  }
}

/* Set all the option flags according to the switches specified.
   Return the index of the first non-option argument.  */

static int
decode_switches (int argc, char **argv)
{
  int c;
  static struct option const long_options[] =
  {
    {"device", required_argument, 0, 'd'},
    {"ifup", required_argument, 0, 'u'},
    {"ifdown", required_argument, 0, 'w'},
    {"delay", required_argument, 0, 'e'},
    {"foreground", no_argument, 0, 'f'},
    {"help", no_argument, 0, 'h'},
    {"version", no_argument, 0, 'V'},
    {NULL, 0, NULL, 0}
  };
  
  while ((c = getopt_long (argc, argv, 
	  "d:"	/* device */
	  "u:"	/* ifup */
	  "w:"	/* ifdown */
	  "e:"	/* delay  */
	  "f"	/* foreground */
	  "h"	/* help */
	  "V",	/* version */
	  long_options, (int *) 0)) != EOF)
  {
    switch (c)
    {
      case 'd':		/* --device  */
	dev = strdup (optarg);
	break;
      case 'u':		/* --ifup  */
	ifup_cmd = strdup (optarg);
	break;
      case 'w':		/* --ifdown  */
	ifdown_cmd = strdup (optarg);
	break;
      case 'e':		/* --ifdown  */
	delay = atoi (optarg);
	break;
      case 'f':		/* --foreground */
	daemonize = 0;
	break;
      case 'V':
	printf ("%s %s\n", basename (program_name), VERSION);
	exit (0);
	
      case 'h':
	usage (0);
	
      default:
	usage (EXIT_FAILURE);
    }
  }
  
  return optind;
}


static void
usage (int status)
{
  printf ("%s - Monitor the link state of Ethernet devices and attempt to\n"
      "bring up or down the interface when the link state changes.\n",
      basename (program_name));
  printf ("Usage: %s [OPTION]...\n", program_name);
  printf ("\
Options:\n\
  -f, --foreground           Do not fork\n\
  -d <dev>, --device <dev>   The device to monitor (defaults to %s)\n\
  -u <cmd>, --ifup <cmd>     Command to run when link state is detected\n\
                             (defaults to %s)\n\
  -w <cmd>, --ifdown <cmd>   Command to run when link state is disappears\n\
                             (defaults to %s)\n\
  -e <secs>, --delay <secs>  Delay this many seconds before believing the\n\
                             change in state (to smooth out transient
			     errors).  Defaults to %d seconds.\n\
  -h, --help                 display this help and exit\n\
  -V, --version              output version information and exit\n\
", DEFAULT_DEV, DEFAULT_IFUP, DEFAULT_IFDOWN, DEFAULT_DELAY);
  exit (status);
}
