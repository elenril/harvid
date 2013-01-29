/*
   sodankyla - video composition daemon

   Copyright (C) 2008 Robin Gareus <robin@gareus.org>

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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <math.h>
#include <sys/stat.h>

#include "daemon_log.h"
#include "daemon_util.h"
#include "socket_server.h"

#ifndef HAVE_WINDOWS
#include <arpa/inet.h> // inet_addr
#endif

extern int debug_level;

#include "jv.h"
#include "v_writer.h"
#include "xjv-ffmpeg.h"
#include "xjv-cache.h"

#ifndef DEFAULT_PORT
#define DEFAULT_PORT 1554
#endif

char *program_name;
int  want_quiet =0;
int  want_verbose =0;
unsigned short  cfg_port = DEFAULT_PORT;
unsigned int  cfg_host = 0; /* = htonl(INADDR_ANY) */
int  cfg_daemonize = 0;
int  cfg_syslog = 0;
char *cfg_logfile = NULL;
char *cfg_chroot = NULL;
char *cfg_username = NULL;
char *cfg_groupname = NULL;
int initial_cache_size = 128;

void printversion (void) {
  printf ("harvid");
  printf (" %s\n", ICSVERSION);
  // TODO: show HTTP-server-version, decoder revision etc.. ?!
}

#include <libgen.h> // basename

void usage (int status) {
  printf ("%s - http ardour video server\n\n", basename(program_name));
  printf ("Usage: %s [OPTION] [document-root]\n", program_name);
  printf ("\n"
"Options:\n"
"  -h, --help                 display this help and exit\n"
"  -V, --version              print version information and exit\n"
"  -q, --quiet, --silent      inhibit usual output\n"
"  -v, --verbose              print more information\n"
"  -s, --syslog               send messages to syslog\n"
"  -P <listenaddr>            IP address to listen on (default 0.0.0.0)\n"
"  -p <num>, --port <num>     TCP port to listen on (default %i)\n"
"  -D, --daemonize            fork into background and detach from tty\n"
"  -c <path>, \n"
"      --chroot <path>        change system root - jails server to this path\n"
"  -l <path>,  \n"
"      --logfile <path>       specify file for log messages\n"
"  -u <name>,\n"
"      --username <name>      server will act as this user\n"
"  -g <name>,\n"
"      --groupname <name>     assume this user-group\n"
"  -C  <frames>               set initial frame-cache size (default: 128)\n"
"  \n"
"if both syslog and logfile are given that last specified option will be used.\n"
"\n"
"Report bugs to <robin@gareus.org>.\n"
"Website http://gareus.org/oss/sodankyla/\n"
, DEFAULT_PORT
);
  exit (status);
}

static struct option const long_options[] =
{
  {"quiet", no_argument, 0, 'q'},
  {"silent", no_argument, 0, 'q'},
  {"verbose", no_argument, 0, 'v'},
  {"help", no_argument, 0, 'h'},
  {"port", required_argument, 0, 'p'},
  {"listenip", required_argument, 0, 'P'},
  {"daemonize", no_argument, 0, 'D'},
  {"chroot", required_argument, 0, 'c'},
  {"logfile", required_argument, 0, 'l'},
  {"syslog", no_argument, 0, 's'},
  {"username", required_argument, 0, 'u'},
  {"groupname", required_argument, 0, 'g'},
  {"version", no_argument, 0, 'V'},
  {"cache-size", required_argument, 0, 'C'},
  {NULL, 0, NULL, 0}
};


/* Set all the option flags according to the switches specified.
   Return the index of the first non-option argument.  */
static int decode_switches (int argc, char **argv) {
  int c;
  while ((c = getopt_long (argc, argv,
         "q"	/* quiet or silent */
         "v"	/* verbose */
         "h"	/* help */
         "p:"	/* port */
         "P:"	/* IP */
         "D"	/* daemonize */
         "c:"	/* chroot-dir */
         "l:"	/* logfile */
         "s"	/* syslog */
         "u:"	/* setUser */
         "g:"	/* setGroup */
         "C:" 	/* initial cache size */
         "V",	/* version */
         long_options, (int *) 0)) != EOF)
  {
    switch (c) {
      case 'q':		/* --quiet, --silent */
        want_quiet = 1;
        want_verbose = 0;
        debug_level=DLOG_ERR;
        break;
      case 'v':		/* --verbose */
        want_verbose = 1;
        if (debug_level==DLOG_INFO)
          debug_level=DLOG_DEBUG;
        else
          debug_level=DLOG_INFO;
        break;
      case 'D':		/* --daemonize */
        cfg_daemonize = 1;
        break;
      case 'l':		/* --logfile */
        cfg_syslog = 0;
        if (cfg_logfile) free(cfg_logfile);
        cfg_logfile = strdup(optarg);
        break;
      case 's':		/* --syslog */
        cfg_syslog = 1;
        if (cfg_logfile) free(cfg_logfile);
        cfg_logfile = NULL;
        break;
      case 'P':		/* --listenip */
        cfg_host = inet_addr (optarg);
        break;
      case 'p':		/* --port */
        {int pn = atoi(optarg);
        if (pn>0 && pn < 65536)
          cfg_port = (unsigned short) atoi(optarg);
        }
        break;
      case 'c':		/* --chroot */
        cfg_chroot = optarg;
        break;
      case 'u':		/* --username */
        cfg_username = optarg;
        break;
      case 'g':		/* --group */
        cfg_groupname = optarg;
        break;
      case 'C':
        initial_cache_size = atoi(optarg);
        if (initial_cache_size < 2 || initial_cache_size > 8192) // XXX what values are sane?
          initial_cache_size = 128;
        break;
      case 'V':
        printversion();
        exit(0);
      case 'h':
        usage (0);
      default:
        usage (1);
    }
  }
  return optind;
}

// -=-=-=-=-=-=- main -=-=-=-=-=-=-
#include "ffcompat.h"

void *dc; // decoder control - TODO make part of daemon struct ?
void *vc = NULL;  // video cache -> hook into dc ? or daemon ?

int main (int argc, char **argv) {
  program_name = argv[0];

  char *docroot = "/" ;

  // TODO: read and apply resource configuration

  debug_level=DLOG_WARNING;

  int i = decode_switches (argc, argv);

  if ((i+1)== argc) docroot = argv[i];
  else if (docroot && i==argc) ; // use default
  else usage(1);

  // TODO: verify configuration ..
  if (cfg_daemonize && !cfg_logfile && !cfg_syslog) {
    dlog(DLOG_WARNING, "daemonizing without log file or syslog.\n");
  }
  // TODO: summarize config

  // go go go

  if (cfg_logfile || cfg_syslog)
    dlog_open(cfg_logfile); // NULL == syslog ;; no dlog_open() -> stderr/out
  if (cfg_chroot) do_chroot(cfg_chroot);
  if (cfg_daemonize) daemonize(); // FIXME: daemonize only after opening socket ?! -> exit(1)

  ff_initialize();

  cache_create(&vc);
  cache_resize(&vc, initial_cache_size);
  decoder_control_create(&dc);

  //cfg_host=htonl(INADDR_ANY);
  //cfg_host=htonl(INADDR_LOOPBACK);
  //cfg_host=inet_addr("127.0.0.1");
  // TODO: start servers as threads
  dlog(DLOG_INFO, "Initialization complete. starting server.\n");
  start_tcp_server(cfg_host, cfg_port, docroot, cfg_username, cfg_groupname, NULL);
  // TODO daemonize() here if more than one has started.
  // and wait until all of them have exited.

  // cleanup

  ff_cleanup();
  decoder_control_destroy(&dc);
  cache_destroy(&vc);
  dlog_close();
  return(0);
}

//  -=-=-=-=-=-=- video server callbacks -=-=-=-=-=-=-
#include "httprotocol.h"
#include "ics_handler.h"

#define STASIZ (262100)
char *server_status_html (CONN *c) {
  char *sm = malloc(STASIZ * sizeof(char));
  int off =0;
  // TODO: built-in style-sheet || optional header from file or/and XSLT
  off+=snprintf(sm+off, STASIZ-off, DOCTYPE HTMLOPEN);
  off+=snprintf(sm+off, STASIZ-off, "<title>ICS Status</title></head>\n<body>\n<h2>ICS - Status</h2>\n\n");
  off+=snprintf(sm+off, STASIZ-off, "<p>status: ok, online.</p>\n");
  off+=snprintf(sm+off, STASIZ-off, "<p>concurrent connections: current/max-seen/limit: %d/%d/%d</p>\n", c->d->num_clients,c->d->max_clients, MAXCONNECTIONS );
  off+=formatdecoderctlinfo(dc, sm+off, STASIZ-off);
  off+=formatcacheinfo(vc, sm+off, STASIZ-off);
  off+=snprintf(sm+off, STASIZ-off, "<hr/><p>sodankyla-ics/%s at %s:%i</p>", ICSVERSION, c->d->local_addr, c->d->local_port);
  off+=snprintf(sm+off, STASIZ-off, "\n</body>\n</html>");
  return (sm);
}

static char *session_info_json (CONN *c, ics_request_args *a, JVINFO *ji) {
  char *im = malloc(256 * sizeof(char));
  int off =0;
  off+=snprintf(im+off,256-off, "{");
//off+=snprintf(im+off,256-off, "\"geometry\":[%i,%i],",ji->movie_width,ji->movie_height);
  off+=snprintf(im+off,256-off, "\"width\":%i",ji->movie_width);
  off+=snprintf(im+off,256-off, ",\"height\":%i",ji->movie_height);
  off+=snprintf(im+off,256-off, ",\"framerate\":%.2f",timecode_rate_to_double(&ji->framerate));
  off+=snprintf(im+off,256-off, ",\"duration\":%"PRId64 ,ji->frames);
  off+=snprintf(im+off,256-off, "}");
  return (im);
}

static char *session_info_html (CONN *c, ics_request_args *a, JVINFO *ji) {
  char *im = malloc(STASIZ * sizeof(char));
  int off =0;
  char smpte[14];
  timecode_framenumber_to_string(smpte, &ji->framerate, ji->frames);

  off+=snprintf(im+off, STASIZ-off, DOCTYPE HTMLOPEN);
  off+=snprintf(im+off, STASIZ-off, "<title>ICS File Info</title></head>\n<body>\n<h2>ICS - Info</h2>\n\n");
  off+=snprintf(im+off, STASIZ-off, "<p>File: %s</p><ul>\n",a->file_name);
  off+=snprintf(im+off, STASIZ-off, "<li>Geometry: %ix%i</li>\n",ji->movie_width, ji->movie_height);
  off+=snprintf(im+off, STASIZ-off, "<li>Framerate: %.2f</li>\n",timecode_rate_to_double(&ji->framerate));
  off+=snprintf(im+off, STASIZ-off, "<li>Duration: %s</li>\n",smpte);
  off+=snprintf(im+off, STASIZ-off, "<li>Duration: %.2f sec</li>\n",(double)ji->frames/timecode_rate_to_double(&ji->framerate));
  off+=snprintf(im+off, STASIZ-off, "<li>Duration: %llu frames</li>\n",(long long unsigned) ji->frames);
  off+=snprintf(im+off, STASIZ-off, "\n</ul>\n</body>\n</html>");
  return (im);
}

static char *session_info_raw (CONN *c, ics_request_args *a, JVINFO *ji) {
  char *im = malloc(STASIZ * sizeof(char));
  int off =0;
  char smpte[14];
  timecode_framenumber_to_string(smpte, &ji->framerate, ji->frames);

  off+=snprintf(im+off, STASIZ-off, "1\n"); // FORMAT VERSION
  off+=snprintf(im+off, STASIZ-off, "%.3f\n",timecode_rate_to_double(&ji->framerate)); // fps
  off+=snprintf(im+off, STASIZ-off, "%llu\n",(long long unsigned) ji->frames); // duration
  off+=snprintf(im+off, STASIZ-off, "0.0\n"); // start-offset TODO
  off+=snprintf(im+off, STASIZ-off, "%f\n",ji->movie_aspect);
  return (im);
}

char *session_info (CONN *c, ics_request_args *a) {
  JVINFO ji;
  init_jvi(&ji);
  int vid;
#ifdef USE_SESSIONXXX // mislabled fn - this is file_info() !!
  jvsession_args sa;
  if (vs_getobj(vs,0, (jv_framenumber) a->frame, &sa)) {
    // TODO: error - session lookup
  }
  vid = jv_get_id(dc, sa.file_name);
   a->frame = sa.frame;
   free_jvs(&sa);
#else
  vid = jv_get_id(dc, a->file_name);
#endif
  jv_get_info(dc, vid, &ji);
  switch (a->render_fmt) {  // XXX make dedicated parameter to select info-format - TODO add xjinfo XML!
    case 2:
      return session_info_raw(c,a,&ji);  // TODO free_jvi()
      break;
    case 1:
      return session_info_json(c,a,&ji);  // TODO: free_jvi();
      break;
    default:
      break;
  }
  return session_info_html(c,a,&ji);
  free_jvi(&ji);
}
/////////////

/*
 * this gets called from protocol_handler() in httprotocol.c
 */
int hardcoded_video(int fd, httpheader *h, ics_request_args *a) {
  JVINFO ji;
  int vid;
  vid = jv_get_id(dc, a->file_name);

  init_jvi(&ji);
  //jv_get_info(dc, vid, &ji);
  // TODO set a->decode_fmt; -- overridden by my_open_movie(..)
  jv_get_info_scale(dc, vid, &ji, a->out_width, a->out_height);
  uint8_t *bptr = cache_get_buffer(vc, vid, a->frame, ji.out_width, ji.out_height);

  uint8_t *optr = NULL;
  long int olen = 0;
  JVARGS out;
  init_jva(&out);
  out.render_fmt = a->render_fmt;

  if (out.render_fmt) {
    // TODO: cache formatted image..
    olen = format_image(&optr, &out, &ji, bptr);
  } else {
    olen = ji.buffersize;
    optr=bptr;
  }

  if(olen>0 && optr) {
    dlog(DLOG_DEBUG, "VID: sending %li bytes to fd:%d.\n", olen, fd);
    switch (out.render_fmt) {
      case 0:
        h->ctype = "image/raw";  // FIXME RGB24
        break;
      case OUT_FMT_JPEG:
        h->ctype = "image/jpeg";
        break;
      case OUT_FMT_PNG:
        h->ctype = "image/png";
        break;
      case OUT_FMT_PPM:
        h->ctype = "image/ppm";
        break;
      default:
        h->ctype = "image/unknown";
    }
    http_tx(fd, 200, h, olen, optr);
    if (out.render_fmt) free(optr); // free formatted image
  } else {
    dlog(DLOG_ERR, "VID: error decoding video file for fd:%d\n",fd);
    httperror(fd, 500, NULL, NULL);
  }
  free_jvi(&ji);
  return (0);
}

void ics_clear_cache() {
  xjv_clear_cache(vc);
}

// vim:sw=2 sts=2 ts=8 et:
