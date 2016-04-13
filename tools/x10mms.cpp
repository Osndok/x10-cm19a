/* Copyright (C) 2004 Michael LeMay
 * Distributed under the GPL
 *
 * Licensed under the Academic Free License version 1.2
 *
 * $Id$
 */

#ifndef X10MMS_CPP
#define X10MMS_CPP

#include <iostream>
#include <fstream>
#include <iterator>
#include <cctype>
#include <xmms/xmmsctrl.h>
#include <map>

bool verbose = false;

const char * DEF_CONF_FNAME = "x10mms.conf";
const char * DEF_DEV_FNAME = "/dev/cm19a0";
const char CONF_ENT_SEP = ':';

const int BALANCE_INCR = 5;
const int VOLUME_INCR = 5;

/**
 * Launch XMMS and connect to it
 */
int forkXmms () throw (std::string) {
  int res = fork();
  if (res == -1) {
    throw std::string("Failed to fork XMMS");
  } else if (res == 0) {
    execlp("xmms", "xmms", NULL);
    throw std::string("Fork succeeded, but failed to execute XMMS");
  } else {
    // Try connecting to XMMS:
    for (int i = 0; i < 8; i++) {
      sleep(1);
      for (int session = 0; session < 16; session++) {
	if (xmms_remote_is_running(session)) {
	  return session;
	}
      }
    }
    throw std::string("Unable to connect with XMMS");
  }
}

typedef void (*XmmsRemoteFn)(gint);

/** Mapping of actions to perform on specific inputs */
XmmsRemoteFn actions[16][16][2];

void xmms_remote_change_volume (gint session, bool increase) {
  int volume = xmms_remote_get_main_volume(session);
  if (increase) {
    volume += VOLUME_INCR;
  } else {
    volume -= VOLUME_INCR;
  }
  xmms_remote_set_main_volume(session, volume);
}

void xmms_remote_increase_volume (gint session) {
  xmms_remote_change_volume(session, true);
}

void xmms_remote_decrease_volume (gint session) {
  xmms_remote_change_volume(session, false);
}

void xmms_remote_change_balance (gint session, bool right) {
  int balance = xmms_remote_get_balance(session);
  if (right) {
    balance += BALANCE_INCR;
  } else {
    balance -= BALANCE_INCR;
  }
  xmms_remote_set_balance(session, balance);
}

void xmms_remote_balance_left (gint session) {
  xmms_remote_change_balance(session, false);
}

void xmms_remote_balance_right (gint session) {
  xmms_remote_change_balance(session, true);
}

void xmms_remote_balance_center (gint session) {
  xmms_remote_set_balance(session, 0);
}

void xmms_remote_toggle_main_win (gint session) {
  xmms_remote_main_win_toggle(session, !xmms_remote_is_main_win(session));
}

void xmms_remote_toggle_pl_win (gint session) {
  xmms_remote_pl_win_toggle(session, !xmms_remote_is_pl_win(session));
}

void xmms_remote_toggle_eq_win (gint session) {
  xmms_remote_eq_win_toggle(session, !xmms_remote_is_eq_win(session));
}

void xmms_remote_aot_toggle (gint session) {
  static bool aot = false;

  xmms_remote_toggle_aot(session, (aot = !aot));
}

bool quit = false;

void xmms_remote_exit (gint session) {
  quit = true;
  
  xmms_remote_quit(session);
}

XmmsRemoteFn strToXmmsFn (const std::string & ent) throw (std::string) {
#define HANDLE(str) \
  if (ent == #str) \
    return xmms_remote_##str
#define ELSE_HANDLE(str) \
  else HANDLE(str)
 
  HANDLE(play);
  ELSE_HANDLE(pause);
  ELSE_HANDLE(stop);
  ELSE_HANDLE(playlist_clear);
  ELSE_HANDLE(increase_volume); /*U*/
  ELSE_HANDLE(decrease_volume); /*U*/
  ELSE_HANDLE(balance_left);    /*U*/
  ELSE_HANDLE(balance_right);   /*U*/
  ELSE_HANDLE(balance_center);  /*U*/
  ELSE_HANDLE(toggle_main_win); /*U*/
  ELSE_HANDLE(toggle_pl_win);   /*U*/
  ELSE_HANDLE(toggle_eq_win);   /*U*/
  ELSE_HANDLE(show_prefs_box);
  //ELSE_HANDLE(show_jump_box);
  ELSE_HANDLE(aot_toggle);      /*U*/
  ELSE_HANDLE(eject);
  ELSE_HANDLE(playlist_prev);
  ELSE_HANDLE(playlist_next);
  ELSE_HANDLE(toggle_repeat);
  ELSE_HANDLE(toggle_shuffle);
  ELSE_HANDLE(exit);
  ELSE_HANDLE(play_pause);
  else throw std::string("Undefined action: " + ent);

#undef ELSE_HANDLE
#undef HANDLE

  return NULL; /*NOTREACHED*/
}

void parseConfEntry (const std::string & ent) throw (std::string) {
  static const char X10_HOUSE_MIN = 'a';
  static const int X10_NUM_MIN = 1;

  int sepPos = ent.find(CONF_ENT_SEP);
  if ((sepPos < 1) || (ent.size() <= sepPos)) {
    throw std::string("Separator in " + ent + " in wrong place");
  }

  std::string cmdStr = ent.substr(0, sepPos);
  std::string actionStr = ent.substr(sepPos+1);
  
  char house = cmdStr[0];
  char * onOffStr;
  int num = strtol(cmdStr.c_str()+1, &onOffStr, 10);
  bool onOff = !strcmp(onOffStr, "on");

  actions[house - X10_HOUSE_MIN][num - X10_NUM_MIN][onOff ? 1 : 0] = strToXmmsFn(actionStr);
}

void parseConfFile (std::istream & inSS) throw (std::string) {
  std::for_each(std::istream_iterator<std::string>(inSS), std::istream_iterator<std::string>(), parseConfEntry);
}

int printHelp (int argCnt, const char * args[]) {
  std::cerr << "Usage: " << args[0] << " [-c " << DEF_CONF_FNAME << "] [-d " << DEF_DEV_FNAME << ']'  << std::endl;
  return 1;
}

int main (int argCnt, const char * args[]) {
  static const char X10_HOUSE_MIN = 'a';
  static const int X10_NUM_MIN = 1;

  std::cout << "+------------------------------+\n"
	    << "| \\  /  -  __                  |\n"
	    << "|  \\/   | /  \\                 |\n"
	    << "|  /\\   | |  |                 |\n"
	    << "| /  \\  | |  |  MMS            |\n"
	    << "|       - \\__/                 |\n"
	    << "+------------------------------+\n"
	    << "| Copyright (C) 2004           |\n"
	    << "|        Michael LeMay         |\n"
	    << "| (See source for license and  |\n"
	    << "|                   credits)   |\n"
	    << "+------------------------------+\n" << std::endl;

  const char * confFName = DEF_CONF_FNAME;
  const char * devFName = DEF_DEV_FNAME;

  int opt;
  while ((opt = getopt(argCnt, (char * const *)args, "vc:d:")) != -1) {
    switch (opt) {
    case 'v':
      verbose = true;
      break;
    case 'c':
      confFName = optarg;
      break;
    case 'd':
      devFName = optarg;
      break;
    default:
      return printHelp(argCnt, args);
    }
  }

  try {
    // Set up action map:
    memset(actions, 0, sizeof(actions));

    if (verbose)
      std::cout << "Reading configuration from " << confFName << "..." << std::flush;
    std::ifstream confSS(confFName);
    parseConfFile(confSS);
    if (verbose)
      std::cout << "done." << std::endl;

    if (verbose)
      std::cout << "Connecting to CM19A..." << std::flush;
    FILE * devFile = fopen(devFName, "r");
    if (devFile == NULL) {
      std::cerr << "Unable to open device file: " << devFName << std::endl;
      return -1;
    }
    if (verbose)
      std::cout << "done" << std::endl;

    if (verbose)
      std::cout << "Forking XMMS..." << std::flush;
    int session = forkXmms();
    if (verbose)
      std::cout << "done." << std::endl;
    
    /*
    std::cout << "Your wish is XMMS' command...\n"
	      << " 5on   < volume >   5off\n"
	      << " 6on   ||      |>   6off\n"
	      << " 7on <stop>  <quit> 7off\n"
	      << " 8on/off: quit remote\n"
	      << std::endl;
    */

    while (!quit) {
      char turnOn;
      char house;
      int num;
      fscanf(devFile, "%c%c%02d\n", &turnOn, &house, &num);
      XmmsRemoteFn action = actions[house - X10_HOUSE_MIN][num - X10_NUM_MIN][(turnOn == '+')? 1 : 0];
      if (action == NULL) {
	if (verbose)
	  std::cout << "Ignoring command: " << turnOn << house << num << std::endl;
      } else {
	action(session);
      }
    }
  } catch (std::string & err) {
    std::cerr << "Caught exception: " << err << std::endl;
    return 1;
  } catch (...) {
    std::cerr << "Unhandled exception, R.I.P." << std::endl;
    return 1;
  }

  return 0;
}

#endif

