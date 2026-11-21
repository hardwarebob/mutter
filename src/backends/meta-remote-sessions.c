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

#include "config.h"

#include "backends/meta-remote-sessions.h"

#include "backends/meta-monitor-manager-private.h"
#include "meta-dbus-remote-sessions.h"

typedef struct _RemoteSessionEntry
{
  char *id;
  char *provider;
  char *client_name;
  char *output_name;
  char *dbus_sender;
  guint watcher_id;
} RemoteSessionEntry;

typedef struct _OutputPolicyEntry
{
  gboolean unlocked_while_locked;
  gboolean always_on;
} OutputPolicyEntry;

struct _MetaRemoteSessions
{
  GObject parent;

  MetaMonitorManager *monitor_manager;
  MetaDBusRemoteSessions *skeleton;
  guint dbus_name_id;
  guint64 next_session_id;

  GPtrArray *sessions;
  GHashTable *output_policies;
};

G_DEFINE_TYPE (MetaRemoteSessions, meta_remote_sessions, G_TYPE_OBJECT)

static void
remote_session_entry_free (gpointer data)
{
  RemoteSessionEntry *entry = data;

  if (!entry)
    return;

  if (entry->watcher_id)
    g_bus_unwatch_name (entry->watcher_id);
  g_free (entry->id);
  g_free (entry->provider);
  g_free (entry->client_name);
  g_free (entry->output_name);
  g_free (entry->dbus_sender);
  g_free (entry);
}

static GVariant *
build_active_sessions_variant (MetaRemoteSessions *self)
{
  GVariantBuilder builder;
  guint i;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));

  for (i = 0; i < self->sessions->len; i++)
    {
      RemoteSessionEntry *entry = g_ptr_array_index (self->sessions, i);
      GVariantBuilder dict;

      g_variant_builder_init (&dict, G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_add (&dict, "{sv}", "id", g_variant_new_string (entry->id));
      g_variant_builder_add (&dict, "{sv}", "provider", g_variant_new_string (entry->provider));
      g_variant_builder_add (&dict, "{sv}", "clientName", g_variant_new_string (entry->client_name));
      g_variant_builder_add (&dict, "{sv}", "outputName", g_variant_new_string (entry->output_name));
      g_variant_builder_add_value (&builder, g_variant_builder_end (&dict));
    }

  return g_variant_builder_end (&builder);
}

static void
sync_active_sessions_property (MetaRemoteSessions *self)
{
  if (!self->skeleton)
    return;

  meta_dbus_remote_sessions_set_active_sessions (self->skeleton,
                                                 build_active_sessions_variant (self));
  meta_dbus_remote_sessions_emit_active_sessions_changed (self->skeleton);
}

static void
remove_session_by_id (MetaRemoteSessions *self,
                      const char         *session_id)
{
  guint i;

  for (i = 0; i < self->sessions->len; i++)
    {
      RemoteSessionEntry *entry = g_ptr_array_index (self->sessions, i);
      if (g_strcmp0 (entry->id, session_id) == 0)
        {
          g_ptr_array_remove_index (self->sessions, i);
          sync_active_sessions_property (self);
          return;
        }
    }
}

static void
on_sender_vanished (GDBusConnection *connection,
                    const char      *name,
                    gpointer         user_data)
{
  MetaRemoteSessions *self = META_REMOTE_SESSIONS (user_data);
  gboolean removed = FALSE;
  guint i;

  for (i = self->sessions->len; i > 0; i--)
    {
      RemoteSessionEntry *entry = g_ptr_array_index (self->sessions, i - 1);
      if (g_strcmp0 (entry->dbus_sender, name) == 0)
        {
          g_ptr_array_remove_index (self->sessions, i - 1);
          removed = TRUE;
        }
    }

  if (removed)
    sync_active_sessions_property (self);
}

static gboolean
handle_register_session (MetaDBusRemoteSessions *skeleton,
                         GDBusMethodInvocation  *invocation,
                         const char             *provider,
                         const char             *client_name,
                         const char             *output_name,
                         gpointer                user_data)
{
  MetaRemoteSessions *self = META_REMOTE_SESSIONS (user_data);
  RemoteSessionEntry *entry;
  const char *sender;

  entry = g_new0 (RemoteSessionEntry, 1);
  entry->id = g_strdup_printf ("session-%" G_GUINT64_FORMAT, ++self->next_session_id);
  entry->provider = g_strdup (provider && *provider ? provider : "Remote");
  entry->client_name = g_strdup (client_name && *client_name ? client_name : "Client");
  entry->output_name = g_strdup (output_name ? output_name : "");

  sender = g_dbus_method_invocation_get_sender (invocation);
  entry->dbus_sender = g_strdup (sender);
  if (sender && *sender)
    {
      entry->watcher_id =
        g_bus_watch_name_on_connection (g_dbus_method_invocation_get_connection (invocation),
                                        sender,
                                        G_BUS_NAME_WATCHER_FLAGS_NONE,
                                        NULL,
                                        on_sender_vanished,
                                        self,
                                        NULL);
    }

  g_ptr_array_add (self->sessions, entry);
  sync_active_sessions_property (self);

  meta_dbus_remote_sessions_complete_register_session (skeleton, invocation, entry->id);
  return TRUE;
}

static gboolean
handle_unregister_session (MetaDBusRemoteSessions *skeleton,
                           GDBusMethodInvocation  *invocation,
                           const char             *session_id,
                           gpointer                user_data)
{
  MetaRemoteSessions *self = META_REMOTE_SESSIONS (user_data);

  remove_session_by_id (self, session_id);
  meta_dbus_remote_sessions_complete_unregister_session (skeleton, invocation);
  return TRUE;
}

static gboolean
handle_request_disconnect (MetaDBusRemoteSessions *skeleton,
                           GDBusMethodInvocation  *invocation,
                           const char             *session_id,
                           gpointer                user_data)
{
  MetaRemoteSessions *self = META_REMOTE_SESSIONS (user_data);

  meta_dbus_remote_sessions_emit_disconnect_requested (self->skeleton,
                                                       session_id ? session_id : "");
  meta_dbus_remote_sessions_complete_request_disconnect (skeleton, invocation);
  return TRUE;
}

static gboolean
handle_set_output_policies (MetaDBusRemoteSessions *skeleton,
                            GDBusMethodInvocation  *invocation,
                            const char             *output_name,
                            gboolean                unlocked_while_locked,
                            gboolean                always_on,
                            gpointer                user_data)
{
  MetaRemoteSessions *self = META_REMOTE_SESSIONS (user_data);

  if (output_name && *output_name)
    {
      if (!unlocked_while_locked && !always_on)
        {
          g_hash_table_remove (self->output_policies, output_name);
        }
      else
        {
          OutputPolicyEntry *policy = g_new0 (OutputPolicyEntry, 1);
          policy->unlocked_while_locked = unlocked_while_locked;
          policy->always_on = always_on;
          g_hash_table_replace (self->output_policies, g_strdup (output_name), policy);
        }

      if (self->skeleton)
        meta_dbus_remote_sessions_emit_output_policies_changed (self->skeleton);

      if (self->monitor_manager)
        meta_monitor_manager_refresh_output_policies (self->monitor_manager);
    }

  meta_dbus_remote_sessions_complete_set_output_policies (skeleton, invocation);
  return TRUE;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const char      *name,
                 gpointer         user_data)
{
  MetaRemoteSessions *self = META_REMOTE_SESSIONS (user_data);
  g_autoptr (GError) error = NULL;

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self->skeleton),
                                         connection,
                                         "/RemoteSessions",
                                         &error))
    {
      g_warning ("Failed to export org.gnome.Mutter.RemoteSessions: %s",
                 error->message);
    }
}

static void
meta_remote_sessions_dispose (GObject *object)
{
  MetaRemoteSessions *self = META_REMOTE_SESSIONS (object);

  g_clear_handle_id (&self->dbus_name_id, g_bus_unown_name);
  g_clear_object (&self->skeleton);
  g_clear_pointer (&self->sessions, g_ptr_array_unref);
  g_clear_pointer (&self->output_policies, g_hash_table_unref);
  self->monitor_manager = NULL;

  G_OBJECT_CLASS (meta_remote_sessions_parent_class)->dispose (object);
}

static void
meta_remote_sessions_class_init (MetaRemoteSessionsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = meta_remote_sessions_dispose;
}

static void
meta_remote_sessions_init (MetaRemoteSessions *self)
{
  self->sessions = g_ptr_array_new_with_free_func (remote_session_entry_free);
  self->output_policies = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->skeleton = meta_dbus_remote_sessions_skeleton_new ();

  g_signal_connect (self->skeleton, "handle-register-session",
                    G_CALLBACK (handle_register_session), self);
  g_signal_connect (self->skeleton, "handle-unregister-session",
                    G_CALLBACK (handle_unregister_session), self);
  g_signal_connect (self->skeleton, "handle-request-disconnect",
                    G_CALLBACK (handle_request_disconnect), self);
  g_signal_connect (self->skeleton, "handle-set-output-policies",
                    G_CALLBACK (handle_set_output_policies), self);

  sync_active_sessions_property (self);

  self->dbus_name_id =
    g_bus_own_name (G_BUS_TYPE_SESSION,
                    "org.gnome.Mutter.RemoteSessions",
                    G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT,
                    on_bus_acquired,
                    NULL,
                    NULL,
                    self,
                    NULL);
}

MetaRemoteSessions *
meta_remote_sessions_new (MetaMonitorManager *monitor_manager)
{
  MetaRemoteSessions *self;

  self = g_object_new (META_TYPE_REMOTE_SESSIONS, NULL);
  self->monitor_manager = monitor_manager;
  return self;
}

gboolean
meta_remote_sessions_get_output_unlocked (MetaRemoteSessions *remote_sessions,
                                          const char         *output_name)
{
  OutputPolicyEntry *policy;

  if (!remote_sessions || !output_name)
    return FALSE;

  policy = g_hash_table_lookup (remote_sessions->output_policies, output_name);
  return policy ? policy->unlocked_while_locked : FALSE;
}

gboolean
meta_remote_sessions_get_output_always_on (MetaRemoteSessions *remote_sessions,
                                           const char         *output_name)
{
  OutputPolicyEntry *policy;

  if (!remote_sessions || !output_name)
    return FALSE;

  policy = g_hash_table_lookup (remote_sessions->output_policies, output_name);
  return policy ? policy->always_on : FALSE;
}

gboolean
meta_remote_sessions_has_any_unlocked_output (MetaRemoteSessions *remote_sessions)
{
  GHashTableIter iter;
  gpointer value;

  if (!remote_sessions)
    return FALSE;

  g_hash_table_iter_init (&iter, remote_sessions->output_policies);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      OutputPolicyEntry *policy = value;
      if (policy->unlocked_while_locked)
        return TRUE;
    }

  return FALSE;
}

gboolean
meta_remote_sessions_has_any_always_on_output (MetaRemoteSessions *remote_sessions)
{
  GHashTableIter iter;
  gpointer value;

  if (!remote_sessions)
    return FALSE;

  g_hash_table_iter_init (&iter, remote_sessions->output_policies);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      OutputPolicyEntry *policy = value;
      if (policy->always_on)
        return TRUE;
    }

  return FALSE;
}
