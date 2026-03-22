#pragma once

//! \file Tracy.h
//! \brief Thin wrapper around Tracy profiler macros.
//! When TRACY_ENABLE is not defined, all macros expand to nothing.

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#define SAMMINE_ZONE_SCOPED ZoneScoped
#define SAMMINE_ZONE_NAMED(name) ZoneScopedN(name)
#define SAMMINE_FRAME_MARK FrameMark
#else
#define SAMMINE_ZONE_SCOPED
#define SAMMINE_ZONE_NAMED(name)
#define SAMMINE_FRAME_MARK
#endif
