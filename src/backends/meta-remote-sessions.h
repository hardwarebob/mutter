/*
 * Copyright (C) 2026 Hermes Contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <glib-object.h>

#include "backends/meta-backend-types.h"

#define META_TYPE_REMOTE_SESSIONS (meta_remote_sessions_get_type ())
G_DECLARE_FINAL_TYPE (MetaRemoteSessions,
                      meta_remote_sessions,
                      META, REMOTE_SESSIONS,
                      GObject)

MetaRemoteSessions * meta_remote_sessions_new (MetaMonitorManager *monitor_manager);

gboolean meta_remote_sessions_get_output_unlocked (MetaRemoteSessions *remote_sessions,
                                                   const char         *output_name);

gboolean meta_remote_sessions_get_output_always_on (MetaRemoteSessions *remote_sessions,
                                                    const char         *output_name);

gboolean meta_remote_sessions_has_any_unlocked_output (MetaRemoteSessions *remote_sessions);

gboolean meta_remote_sessions_has_any_always_on_output (MetaRemoteSessions *remote_sessions);
