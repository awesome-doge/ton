// SPDX-License-Identifier: LGPL-2.1-or-later
// Extended logging utility for TON Node diagnostics
// Author: Marta Nowak
// Date: 2025-11-01

#include "common/log_extended.h"
#include <ctime>
#include <iomanip>
#include <sstream>

namespace ton {
namespace log {

std::string extended_timestamp() {
  auto now = std::chrono::system_clock::now();
  auto time = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

  std::ostringstream oss;
  oss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S")
      << "." << std::setw(3) << std::setfill('0') << ms.count();
  return oss.str();
}

void info_with_timestamp(const std::string& msg) {
  std::cout << "[INFO " << extended_timestamp() << "] " << msg << std::endl;
}

}  // namespace log
}  // namespace ton
