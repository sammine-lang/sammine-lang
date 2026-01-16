//
// Created by Claude Code
//

#include "util/Logging.h"
#include <string>

namespace sammine_log {
namespace {
// Minimal global: just the command-line string
std::string g_enabled_types;
} // anonymous namespace

void set_enabled_types(const std::string &types_str) {
  g_enabled_types = types_str;
}

const std::string &get_enabled_types() { return g_enabled_types; }

bool is_type_in_list(const char *type, const std::string &list) {
  if (list.empty())
    return false;
  std::string search = std::string(";") + type + ";";
  std::string haystack = ";" + list + ";";
  return haystack.find(search) != std::string::npos;
}
} // namespace sammine_log
