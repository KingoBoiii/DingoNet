#pragma once

#include <string_view>

// A Windows host and a Linux client exchange packets by hand: --tcp-host=<ip>:<port> on one machine,
// --tcp-client=<ip>:<port> on the other. --lan-host[=port] and --lan-browse[=port] check LAN discovery by
// broadcast. -1 when the argument is none of these.
int RunNetCheck(std::string_view argument);
