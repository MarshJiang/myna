// SPDX-License-Identifier: GPL-3.0-or-later

#include "settings.h"

GSettings *
myna_settings_new (void)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
  GSettingsSchema *schema;
  GSettings *settings;

  if (!source)
    return NULL;

  schema = g_settings_schema_source_lookup (source, MYNA_SETTINGS_SCHEMA, TRUE);
  if (!schema)
    return NULL;

  settings = g_settings_new_full (schema, NULL, NULL);
  g_settings_schema_unref (schema);
  return settings;
}
