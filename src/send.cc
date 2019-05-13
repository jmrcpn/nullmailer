// nullmailer -- a simple relay-only MTA
// Copyright (C) 2018  Bruce Guenter <bruce@untroubled.org>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// You can contact me at <bruce@untroubled.org>.  There is also a mailing list
// available to discuss this package.  To subscribe, send an email to
// <nullmailer-subscribe@lists.untroubled.org>.

#include "config.h"
#include <sstream>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "ac/time.h"
#include "argparse.h"
#include "autoclose.h"
#include "configio.h"
#include "defines.h"
#include "errcodes.h"
#include "fdbuf/fdbuf.h"
#include "cli++/cli++.h"
#include "forkexec.h"
#include "hostname.h"
#include "itoa.h"
#include "list.h"
#include "selfpipe.h"
#include "setenv.h"

const char* cli_program = "nullmailer-send";
const char* cli_help_prefix = "nullmailer daemon\n";
const char* cli_help_suffix = "";
const char* cli_args_usage  = "";
const int cli_args_min = 0;
const int cli_args_max = 0;

cli_option cli_options[] = {
  { 'd', "daemon", cli_option::flag, 1, &daemonize,  "daemonize , implies --syslog", 0 },
  { 's', "syslog", cli_option::flag, 1, &use_syslog, "use syslog", 0 },
  { 0, 0, cli_option::flag, 0, 0, 0, 0 }
};



selfpipe selfpipe;

typedef enum { tempfail=-1, permfail=0, success=1 } tristate;

struct message
{
  time_t timestamp;
  mystring filename;
};

typedef list<mystring> slist;
typedef list<struct message> msglist;

static mystring trigger_path;
static mystring msg_dir;

struct remote
{
  static const mystring default_proto;
  
  mystring host;
  mystring proto;
  mystring program;
  mystring options;
  remote(const slist& list);
  ~remote();
};

const mystring remote::default_proto = "smtp";

remote::remote(const slist& lst)
{
  slist::const_iter iter = lst;
  host = *iter;
  options = "host=" + host + "\n";
  ++iter;
  if(!iter)
    proto = default_proto;
  else {
    proto = *iter;
    for(++iter; iter; ++iter) {
      mystring option = *iter;
      // Strip prefix "--"
      if (option[0] == '-' && option[1] == '-')
	option = option.right(2);
      options += option;
      options += '\n';
    }
  }
  options += "-d\n";
  if (use_syslog==true)
    options += "-s\n";
  options += '\n';
  program = CONFIG_PATH(PROTOCOLS, NULL, proto.c_str());
}

remote::~remote() { }

typedef list<remote> rlist;

static rlist remotes;
static int minpause = 60;
static int pausetime = minpause;
static int maxpause = 24*60*60;
static int sendtimeout = 60*60;
static int queuelifetime = 7*24*60*60;

//***********************************************************************
bool load_remotes()
{
  slist rtmp;
  config_readlist("remotes", rtmp);
  remotes.empty();
  for(slist::const_iter r(rtmp); r; r++) {
    if((*r)[0] == '#')
      continue;
    arglist parts;
    if (!parse_args(parts, *r))
      continue;
    remotes.append(remote(parts));
  }
  if (remotes.count() == 0)
    (void) report("No remote hosts listed for delivery");
  return true;
}

bool load_config()
{
  mystring hh;

  if (!config_read("helohost", hh))
    hh = me;
  setenv("HELOHOST", hh.c_str(), 1);

  int oldminpause = minpause;
  if(!config_readint("pausetime", minpause))
    minpause = 60;
  if(!config_readint("maxpause", maxpause))
    maxpause = 24*60*60;
  if(!config_readint("sendtimeout", sendtimeout))
    sendtimeout = 60*60;
  if(!config_readint("queuelifetime", queuelifetime))
    queuelifetime = 7*24*60*60;

  if (minpause != oldminpause)
    pausetime = minpause;

  return load_remotes();
}

static msglist messages;
static bool reload_messages = false;

void catch_alrm(int)
{
  signal(SIGALRM, catch_alrm);
  reload_messages = true;
}

bool load_messages()
{
  reload_messages = false;
  (void) report("Rescanning queue.");
  DIR* dir = opendir(".");
  if(!dir) {
    (void) reporterror("Cannot open queue directory: ",strerror(errno));
    return false;
    }
  messages.empty();
  struct dirent* entry;
  while((entry = readdir(dir)) != 0) {
    const char* name = entry->d_name;
    if (name[0] == '.')
      continue;
    struct stat st;
    if (stat(name, &st) < 0) {
      fout << "Could not stat " << name << ", skipping." << endl;
      continue;
    }
    struct message m = { st.st_mtime, name };
    messages.append(m);
  }
  closedir(dir);
  return true;
}

tristate catchsender(fork_exec& fp)
{
  for (;;) {
    switch (selfpipe.waitsig(sendtimeout)) {
    case 0:			// timeout
      (void) report("Sending timed out, killing protocol");
      fp.kill(SIGTERM);
      selfpipe.waitsig();	// catch the signal from killing the child
      return tempfail;
      break;
    case -1:
      (void) reporterror("Error waiting for the child signal: ",strerror(errno));
      return permfail;
      break;
    case SIGCHLD:
      break;
    default:
      continue;
    }
    break;
  }

  int status = fp.wait_status();
  if(status < 0) {
    fout << "Error catching the child process return value: "
	 << strerror(errno) << endl;
    return tempfail;
  }
  else {
    if(WIFEXITED(status)) {
      status = WEXITSTATUS(status);
      if(status) {
        (void) reporterror("Sending failed: ",errorstr(status));
	return (status & ERR_PERMANENT_FLAG) ? permfail : tempfail;
      }
      else {
	(void) report("Sent file.");
	return success;
      }
    }
    else {
      (void) report("Sending process crashed or was killed.");
      return tempfail;
    }
  }
}

bool log_msg(mystring& filename, remote& remote, int fd)
{
  mystring ss;


  ss  = "Starting delivery:";
  ss += " host: " + remote.host;
  ss += " protocol: " + remote.proto;
  ss += " file: " + filename;
  (void) report(ss);
  fdibuf in(fd);
  mystring line;
  mystring msg;
  if (in.getline(line, '\n')) {
    msg = "From: <";
    msg += line;
    msg += '>';
    bool has_to = false;
    while (in.getline(line, '\n')) {
      if (!line)
        break;
      msg += has_to ? ", " : " to: ";
      has_to = true;
      msg += '<';
      msg += line;
      msg += '>';
    }
    (void) report(msg);
    while (in.getline(line, '\n')) {
      if (!line)
        break;
      if (line.left(11).lower() == "message-id:")
        (void) report(line);
    }
    lseek(fd, 0, SEEK_SET);
    return true;
  }
  (void) report("Error, Can't read message");
  return false;
}

static bool copy_output(int fd, mystring& output)
{
  output = "";
  char buf[256];
  ssize_t rd;
  while ((rd = read(fd, buf, sizeof buf)) > 0)
    output += mystring(buf, rd);
  return rd == 0;
}

tristate send_one(mystring filename, remote& remote, mystring& output)
{
  autoclose fd = open(filename.c_str(), O_RDONLY);
  if(fd < 0) {
    fout << "Can't open file '" << filename << "'" << endl;
    return tempfail;
  }
  log_msg(filename, remote, fd);

  fork_exec fp(remote.proto.c_str());
  int redirs[] = { REDIRECT_PIPE_TO, REDIRECT_PIPE_FROM, REDIRECT_NONE, fd };
  if (!fp.start(remote.program.c_str(), 4, redirs))
    return tempfail;

  if (write(redirs[0], remote.options.c_str(), remote.options.length()) != (ssize_t)remote.options.length())
    (void) report("Warning: Writing options to protocol failed");
  close(redirs[0]);

  tristate result = catchsender(fp);
  if (!copy_output(redirs[1], output))
    (void) report("Warning: Could not read output from protocol");
  close(redirs[1]);
  return result;
}

static void parse_output(const mystring& output, const remote& remote, mystring& status, mystring& diag)
{
  diag = remote.proto.upper();
  diag += "; ";
  diag += output.strip();
  diag.subst('\n', '/');
  status = "5.0.0";
  for (unsigned i = 0; i < output.length()-5; i++)
    if (isdigit(output[i])
        && output[i+1] == '.'
        && isdigit(output[i+2])
        && output[i+3] == '.'
        && isdigit(output[i+4])) {
      status = output.sub(i, 5);
      break;
    }
}

static bool is_bounce(int fd)
{
  fdibuf in(fd);
  mystring sender;
  bool result = in.getline(sender) && sender.length() == 0;
  lseek(fd, 0, SEEK_SET);
  return result;
}

bool bounce_msg(const message& msg, const remote& remote, const mystring& output)
{
  mystring failed = "../failed/";
  failed += msg.filename;
  fout << "Moving message " << msg.filename << " into failed" << endl;
  if (rename(msg.filename.c_str(), failed.c_str()) == -1) {
    fout << "Can't rename file: " << strerror(errno) << endl;
    return false;
  }
  autoclose fd = open(failed.c_str(), O_RDONLY);
  if (fd < 0)
    fout << "Can't open file " << failed << " to create bounce message" << endl;
  else if (is_bounce(fd))
    fout << "Not generating double bounce for " << msg.filename << "" << endl;
  else {
    fout << "Generating bounce for " << msg.filename << endl;
    queue_pipe qp;
    autoclose pfd = qp.start();
    if (pfd > 0) {
      mystring program = program_path("nullmailer-dsn");
      fork_exec dsn("nullmailer-dsn");
      int redirs[] = { fd, pfd };
      mystring status_code, diag_code;
      parse_output(output, remote, status_code, diag_code);
      const char* args[] = { program.c_str(),
                             "--last-attempt", itoa(time(NULL)),
                             "--remote", remote.host.c_str(),
                             "--diagnostic-code", diag_code.c_str(),
                             status_code.c_str(), NULL };
      dsn.start(args, 2, redirs);
      // Everything else cleans up itself
    }
  }
  return true;
}

void send_all()
{
  std::stringstream ss;
  if(!load_config()) {
    (void) report("Could not load the config");
    return;
  }
  if(remotes.count() <= 0) {
    (void) report("No remote hosts listed for delivery");
    return;
  }
  if(messages.count() == 0)
    return;
  ss << "Starting delivery, " << itoa(messages.count()) << " message(s) in queue.";
  (void) report(ss.str().c_str());
  mystring output;
  for(rlist::iter remote(remotes); remote; remote++) {
    msglist::iter msg(messages);
    while(msg) {
      switch (send_one((*msg).filename, *remote, output)) {
      case tempfail:
	if (time(0) - (*msg).timestamp > queuelifetime) {
	  if (bounce_msg(*msg, *remote, output)) {
	    messages.remove(msg);
	    continue;
	  }
	}
	msg++;
	break;
      case permfail:
	if (bounce_msg(*msg, *remote, output))
	  messages.remove(msg);
	else
	  msg++;
	break;
      default:
	if(unlink((*msg).filename.c_str()) == -1) {
	  fout << "Can't unlink file: " << strerror(errno) << endl;
	  msg++;
	}
	else
	  messages.remove(msg);
      }
    }
  ss.str("");
  ss << "Delivery complete, "
       << itoa(messages.count()) << " message(s) remain.";
  (void) report(ss.str().c_str());
  }
}

static int trigger;
#ifdef NAMEDPIPEBUG
static int trigger2;
#endif

bool open_trigger()
{
  trigger = open(trigger_path.c_str(), O_RDONLY|O_NONBLOCK);
#ifdef NAMEDPIPEBUG
  trigger2 = open(trigger_path.c_str(), O_WRONLY|O_NONBLOCK);
#endif
  if(trigger == -1) {
    (void) reporterror("JMPDBG trigger: ",trigger_path.c_str());
    (void) reporterror("Could not open trigger file: ",strerror(errno));
    return false;
    }
  return true;
}

bool read_trigger()
{
  if(trigger != -1) {
    char buf[1024];
    read(trigger, buf, sizeof buf);
#ifdef NAMEDPIPEBUG
    close(trigger2);
#endif
    close(trigger);
  }
  return open_trigger();
}

bool do_select()
{
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(trigger, &readfds);
  struct timeval timeout;

  if (messages.count() == 0)
    pausetime = maxpause;
  timeout.tv_sec = pausetime;
  timeout.tv_usec = 0;

  pausetime *= 2;
  if (pausetime > maxpause)
    pausetime = maxpause;

  int s = select(trigger+1, &readfds, 0, 0, &timeout);
  if(s == 1) {
    (void) report("Trigger pulled.");
    read_trigger();
    reload_messages = true;
    pausetime = minpause;
  }
  else if(s == -1 && errno != EINTR) {
    (void) reporterror("Internal error in select: ",strerror(errno));
    return false;
    }
  else if(s == 0)
    reload_messages = true;
  if(reload_messages)
    load_messages();
  return true;
}

int cli_main(int, char*[])
{
  pid_t pid;
  trigger_path = CONFIG_PATH(QUEUE, NULL, "trigger");
  msg_dir = CONFIG_PATH(QUEUE, NULL, "queue");

  if (daemonize==true)
    use_syslog=true;
  if (use_syslog==true)
    (void) openlog(cli_program,LOG_PID,LOG_MAIL);
  read_hostnames();

  if (!selfpipe) {
    (void) report("Could not set up self-pipe.");
    return 1;
  }
  selfpipe.catchsig(SIGCHLD);

  if (daemonize==true) {
   switch (pid=fork()) {
     case -1	:	/*unable to fork	*/
       (void) syslog(LOG_CRIT, "Could not fork.");
       return 1;
       break;
     case 0	:	/*the forked process	*/
       (void) close(STDIN_FILENO);
       (void) close(STDOUT_FILENO);
       (void) close(STDERR_FILENO);
       break;
     default	:	/*The parent process	*/
       return 0;
       break;
     }
   }
  if (!open_trigger()) {
    (void) report("Could not open trigger");
    return 1;
    }
  if(chdir(msg_dir.c_str()) == -1) {
    (void) report("Could not chdir to queue message directory.");
    return 1;
    }
  signal(SIGALRM, catch_alrm);
  signal(SIGHUP, SIG_IGN);
  if (daemon_lock(cli_program,true)==true) {
    load_config();
    load_messages();
    for(;;) {
      send_all();
      if (minpause == 0) break;
      do_select();
    }
    (void) daemon_lock(cli_program,false);
    }
  return 0;
}
