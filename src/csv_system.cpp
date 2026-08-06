#include "csv_system.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace csv {
namespace {

// Linux exposes a genuine "how much can you allocate without swapping"
// figure, which is far more useful than free memory.
size_t MemAvailableFromProc() {
  std::ifstream meminfo("/proc/meminfo");
  if (!meminfo.is_open())
    return 0;

  std::string line;
  while (std::getline(meminfo, line)) {
    if (line.rfind("MemAvailable:", 0) != 0)
      continue;
    std::istringstream parser(line.substr(13));
    unsigned long long kilobytes = 0;
    if (parser >> kilobytes)
      return static_cast<size_t>(kilobytes) * 1024;
    return 0;
  }
  return 0;
}

} // namespace

size_t AvailableMemoryBytes() {
  // An explicit ceiling wins over anything the system reports: it lets users
  // on shared machines cap csvtui, and makes the guard testable.
  if (const char *limit = std::getenv("CSVTUI_MEMORY_LIMIT")) {
    char *stop = nullptr;
    const unsigned long long value = std::strtoull(limit, &stop, 10);
    if (stop != limit && value > 0)
      return static_cast<size_t>(value);
  }

  if (const size_t from_proc = MemAvailableFromProc())
    return from_proc;

#if defined(__APPLE__)
  // Free plus inactive pages is the closest analogue to MemAvailable.
  vm_size_t page_size = 0;
  mach_port_t host = mach_host_self();
  if (host_page_size(host, &page_size) == KERN_SUCCESS) {
    vm_statistics64_data_t stats{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host, HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&stats),
                          &count) == KERN_SUCCESS) {
      const unsigned long long pages =
          static_cast<unsigned long long>(stats.free_count) + stats.inactive_count;
      return static_cast<size_t>(pages * page_size);
    }
  }
#endif

#if defined(_SC_AVPHYS_PAGES) && defined(_SC_PAGESIZE)
  const long pages = ::sysconf(_SC_AVPHYS_PAGES);
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (pages > 0 && page_size > 0)
    return static_cast<size_t>(pages) * static_cast<size_t>(page_size);
#endif

  return 0; // unknown
}

std::string HumanBytes(size_t bytes) {
  static const char *const units[] = {"B", "kB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
    value /= 1024.0;
    ++unit;
  }

  char buffer[64];
  if (unit == 0)
    std::snprintf(buffer, sizeof(buffer), "%.0f %s", value, units[unit]);
  else if (value < 10.0)
    std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, units[unit]);
  else
    std::snprintf(buffer, sizeof(buffer), "%.0f %s", value, units[unit]);
  return buffer;
}

std::string HumanCount(size_t value) {
  const std::string digits = std::to_string(value);
  std::string out;
  int since = 0;
  for (size_t i = digits.size(); i-- > 0;) {
    out.push_back(digits[i]);
    if (++since == 3 && i > 0) {
      out.push_back(' ');
      since = 0;
    }
  }
  std::reverse(out.begin(), out.end());
  return out;
}

} // namespace csv
