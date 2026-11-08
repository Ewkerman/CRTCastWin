// Stand-in for Groovy_MiSTer's api/rio.h when building with MSVC.
// Upstream rio.h declares the Registered I/O (RIO) types by hand because MinGW lacks them.
// The Windows SDK already declares them in <mswsock.h> (Windows 8+), and the hand-written copies
// clash with the SDK's, so with MSVC we just use the SDK's. groovymister.cpp stays unchanged.
#pragma once
#include <winsock2.h>
#include <mswsock.h>
// Upstream rio.h also brings these in, and groovymister.cpp relies on them (std::numeric_limits, std::min).
#include <algorithm>
#include <iostream>
#include <limits>
