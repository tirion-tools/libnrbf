// SPDX-License-Identifier: MIT
// Optional Tracy zone macros. The consumer's build sets TRACY_ENABLE
// and the Tracy include path; standalone builds get no-ops.
#pragma once

#ifdef TRACY_ENABLE
  #include <tracy/Tracy.hpp>
  #define NRBF_ZONE()        ZoneScoped
  #define NRBF_ZONE_N(name)  ZoneScopedN(name)
#else
  #define NRBF_ZONE()        do {} while (0)
  #define NRBF_ZONE_N(name)  do {} while (0)
#endif
