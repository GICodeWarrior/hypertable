/**
 * Copyright (C) 2007 Doug Judd (Zvents, Inc.)
 * 
 * This file is part of Hypertable.
 * 
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 * 
 * Hypertable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

extern "C" {
#include <readline/readline.h>
#include <readline/history.h>
}

#include "Common/InteractiveCommand.h"
#include "Common/Properties.h"
#include "Common/System.h"
#include "Common/Usage.h"

#include "AsyncComm/Comm.h"
#include "AsyncComm/ConnectionManager.h"

#include "Hypertable/Lib/Manager.h"
#include "Hypertable/Lib/RangeServerClient.h"
#include "Hyperspace/HyperspaceClient.h"

#include "CommandCreateScanner.h"
#include "CommandFetchScanblock.h"
#include "CommandLoadRange.h"
#include "CommandUpdate.h"
#include "Global.h"

using namespace hypertable;
using namespace std;

namespace {

  char *line_read = 0;

  char *rl_gets () {

    if (line_read) {
      free (line_read);
      line_read = (char *)NULL;
    }

    /* Get a line from the user. */
    line_read = readline ("rsclient> ");

    /* If the line has any text in it, save it on the history. */
    if (line_read && *line_read)
      add_history (line_read);

    return line_read;
  }

  /**
   * Parses the --server= command line argument.
   */
  void ParseServerArgument(const char *arg, std::string &host, uint16_t *portPtr) {
    const char *ptr = strchr(arg, ':');
    *portPtr = 0;
    if (ptr) {
      host = string(arg, ptr-arg);
      *portPtr = (uint16_t)atoi(ptr+1);
    }
    else
      host = arg;
    return;
  }


  /**
   *
   */
  void BuildInetAddress(struct sockaddr_in &addr, PropertiesPtr &propsPtr, std::string &userSuppliedHost, uint16_t userSuppliedPort) {
    std::string host;
    uint16_t port;

    if (userSuppliedHost == "")
      host = "localhost";
    else
      host = userSuppliedHost;

    if (userSuppliedPort == 0)
      port = (uint16_t)propsPtr->getPropertyInt("Hypertable.RangeServer.port", 38549);
    else
      port = userSuppliedPort;

    memset(&addr, 0, sizeof(struct sockaddr_in));
    {
      struct hostent *he = gethostbyname(host.c_str());
      if (he == 0) {
	herror("gethostbyname()");
	exit(1);
      }
      memcpy(&addr.sin_addr.s_addr, he->h_addr_list[0], sizeof(uint32_t));
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
  }


  const char *usage[] = {
    "usage: hypertable [OPTIONS]",
    "",
    "OPTIONS:",
    "",
    "  --config=<file>   Read configuration from <file>.  The default config",
    "     file is \"conf/hypertable.cfg\" relative to the toplevel install",
    "     directory",
    "",
    "  --server=<host>[:<port>]  Connect to Range server at machine <host>",
    "     and port <port>.  By default it connects to localhost:38549.",
    "",
    "  --help  Display this help text and exit.",
    "",
    "This program provides a command line interface to a Range Server.",
    (const char *)0
  };

  const char *helpHeader[] = {
    "",
    "The following commands can be used to send requests to a Range Server.",
    "In the command descriptions below, the argument <range> has the format",
    "<table>[<startRow>:<endRow>].  For example, to specify a range from",
    "table Test that has a start row of 'carrot' and an end row of 'kerchoo',",
    "you would type the following:",
    "",
    "  Test[carrot:kerchoo]",
    "",
    "To specify a range that starts at the beginning of a table or one that",
    "spans to the end of a table, you can omit the row key as follows:",
    "",
    "  Test[carrot:]",
    "  Test[:kerchoo]",
    "",
    "",
    "Command Summary",
    "---------------",
    "",
    (const char *)0
  };

  const char *helpTrailer[] = {
    "help",
    "",
    "  Display this help text.",
    "",
    "quit",
    "",
    "  Exit the program.",
    "",
    (const char *)0
  };
  
}


/**
 *
 */
int main(int argc, char **argv) {
  const char *line;
  string configFile = "";
  string host = "";
  uint16_t port = 0;
  struct sockaddr_in addr;
  vector<InteractiveCommand *>  commands;
  Comm *comm;
  ConnectionManager *connManager;
  PropertiesPtr propsPtr;
  CommandFetchScanblock *fetchScanblock;

  System::Initialize(argv[0]);
  ReactorFactory::Initialize((uint16_t)System::GetProcessorCount());

  for (int i=1; i<argc; i++) {
    if (!strncmp(argv[i], "--config=", 9))
      configFile = &argv[i][9];
    else if (!strncmp(argv[i], "--server=", 9))
      ParseServerArgument(&argv[i][9], host, &port);
    else
      Usage::DumpAndExit(usage);
  }

  if (configFile == "")
    configFile = System::installDir + "/conf/hypertable.cfg";

  propsPtr.reset( new Properties(configFile) );

  BuildInetAddress(addr, propsPtr, host, port);  

  comm = new Comm();
  connManager = new ConnectionManager(comm);

  // Create Range Server client object
  Global::rangeServer = new RangeServerClient(connManager);

  // Connect to Range Server
  connManager->Add(addr, 30, "Range Server");
  if (!connManager->WaitForConnection(addr, 15))
    cerr << "Timed out waiting for for connection to Range Server.  Exiting ..." << endl;

  // Connect to Master
  Global::master = new MasterClient(connManager, propsPtr);
  if (!Global::master->WaitForConnection(15))
    cerr << "Timed out waiting for for connection to Master.  Exiting ..." << endl;

  // Connect to Hyperspace
  Global::hyperspace = new HyperspaceClient(connManager, propsPtr.get());
  if (!Global::hyperspace->WaitForConnection(30))
    exit(1);

  commands.push_back( new CommandCreateScanner(addr) );  
  fetchScanblock = new CommandFetchScanblock(addr);
  commands.push_back( fetchScanblock );
  commands.push_back( new CommandLoadRange(addr) );
  commands.push_back( new CommandUpdate(addr) );

  cout << "Welcome to the Range Server command interpreter." << endl;
  cout << "Type 'help' for a description of commands." << endl;
  cout << endl << flush;

  using_history();
  while ((line = rl_gets()) != 0) {
    size_t i;

    if (*line == 0) {
      if (Global::outstandingScannerId != -1) {
	fetchScanblock->ClearArgs();
	fetchScanblock->run();
      }
      continue;
    }

    for (i=0; i<commands.size(); i++) {
      if (commands[i]->Matches(line)) {
	commands[i]->ParseCommandLine(line);
	commands[i]->run();
	break;
      }
    }

    if (i == commands.size()) {
      if (!strcmp(line, "quit") || !strcmp(line, "exit"))
	exit(0);
      else if (!strcmp(line, "help")) {
	Usage::Dump(helpHeader);
	for (i=0; i<commands.size(); i++) {
	  Usage::Dump(commands[i]->Usage());
	  cout << endl;
	}
	Usage::Dump(helpTrailer);
      }
      else
	cout << "Unrecognized command." << endl;
    }

  }

  return 0;
}