#pragma once
#include <iosfwd>
#include <string>
#include <vector>

namespace hawken {
int run_cli(const std::vector<std::string>& args, std::ostream& output, std::ostream& error);
}
