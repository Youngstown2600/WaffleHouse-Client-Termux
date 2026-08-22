#pragma once

#include <QtGlobal>

namespace UserActivity {
// Returns the best available workstation idle time in milliseconds.
// On X11, xprintidle is used when available. Otherwise the caller's last
// WaffleHouse input timestamp is used as a portable fallback.
qint64 idleMilliseconds(qint64 lastApplicationActivityMs);
}
