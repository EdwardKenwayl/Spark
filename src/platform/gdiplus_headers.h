#pragma once

#include <algorithm>
#include <objidl.h>

using byte = BYTE;
#ifdef SPARK_WINDOWS_XP
// The 7.1A GDI+ headers expect unqualified min/max with NOMINMAX enabled.
namespace Gdiplus {
using std::min;
using std::max;
}
#endif
#include <gdiplus.h>
