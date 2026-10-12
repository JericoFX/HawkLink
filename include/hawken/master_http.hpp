#pragma once
#include "hawken/master_directory.hpp"

namespace httplib { class Server; }
namespace hawken::master {
// The immutable directory must outlive the listener. This installs read-only routes.
void configure_master_http(httplib::Server& server, const MasterDirectory& directory);
}
