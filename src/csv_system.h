#pragma once

#include <cstddef>
#include <string>

namespace csv {

// Memory the process could plausibly allocate right now, in bytes. Returns 0
// when the platform cannot answer, which callers must treat as "unknown" and
// not as "none".
size_t AvailableMemoryBytes();

// "9.3 GB", "742 MB", "12 kB" — for messages shown to the user.
std::string HumanBytes(size_t bytes);

// "1 234 567" with thin separators, for row counts.
std::string HumanCount(size_t value);

} // namespace csv
