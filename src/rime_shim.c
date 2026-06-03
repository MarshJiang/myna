#include "rime_shim.h"

#include <glib.h>
#include <rime_api.h>
#include <string.h>

struct MynaRime {
  RimeApi *api;
  RimeSessionId session;
  char *pending_commit;
};

static gboolean rime_ready;

static void
select_preferred_schema (MynaRime *self)
{
  static const char *schemas[] = {
    "luna_pinyin_simp",
    "luna_pinyin",
    "double_pinyin",
  };

  for (guint i = 0; i < G_N_ELEMENTS (schemas); i++) {
    if (self->api->select_schema (self->session, schemas[i]))
      return;
  }
}

static char *
dup_or_null (const char *text)
{
  if (!text || !*text)
    return NULL;
  return g_strdup (text);
}

MynaRime *
myna_rime_create (void)
{
  RimeApi *api = rime_get_api ();

  if (!rime_ready) {
    RIME_STRUCT (RimeTraits, traits);
    traits.shared_data_dir = "/usr/share/rime-data";
    traits.user_data_dir = g_build_filename (g_get_user_config_dir (), "myna-rime", NULL);
    traits.distribution_name = "Myna";
    traits.distribution_code_name = "myna";
    traits.distribution_version = "0.1.0";

    api->setup (&traits);
    api->initialize (&traits);
    api->start_maintenance (true);
    api->join_maintenance_thread ();

    g_free ((char *) traits.user_data_dir);
    rime_ready = TRUE;
  }

  MynaRime *self = g_new0 (MynaRime, 1);
  self->api = api;
  self->session = api->create_session ();
  select_preferred_schema (self);
  return self;
}

void
myna_rime_destroy (MynaRime *self)
{
  if (!self)
    return;

  if (self->session)
    self->api->destroy_session (self->session);
  g_clear_pointer (&self->pending_commit, g_free);
  g_free (self);
}

bool
myna_rime_process_key (MynaRime *self, int key)
{
  if (!self || !self->session)
    return false;

  gboolean handled = self->api->process_key (self->session, key, 0);

  RIME_STRUCT (RimeCommit, commit);
  if (self->api->get_commit (self->session, &commit)) {
    g_free (self->pending_commit);
    self->pending_commit = dup_or_null (commit.text);
    self->api->free_commit (&commit);
  }

  return handled;
}

bool
myna_rime_select_candidate (MynaRime *self, size_t index)
{
  if (!self || !self->session)
    return false;

  if (!self->api->select_candidate (self->session, (int) index))
    return false;

  RIME_STRUCT (RimeCommit, commit);
  if (self->api->get_commit (self->session, &commit)) {
    g_free (self->pending_commit);
    self->pending_commit = dup_or_null (commit.text);
    self->api->free_commit (&commit);
  }

  return true;
}

void
myna_rime_clear (MynaRime *self)
{
  if (!self || !self->session)
    return;

  self->api->clear_composition (self->session);
  g_clear_pointer (&self->pending_commit, g_free);
}

char *
myna_rime_take_commit (MynaRime *self)
{
  if (!self || !self->pending_commit)
    return NULL;

  char *commit = self->pending_commit;
  self->pending_commit = NULL;
  return commit;
}

char *
myna_rime_get_preedit (MynaRime *self)
{
  if (!self || !self->session)
    return NULL;

  RIME_STRUCT (RimeContext, context);
  if (!self->api->get_context (self->session, &context))
    return NULL;

  char *preedit = NULL;
  if (context.composition.length > 0)
    preedit = dup_or_null (context.composition.preedit);

  self->api->free_context (&context);
  return preedit;
}

size_t
myna_rime_get_candidate_count (MynaRime *self)
{
  if (!self || !self->session)
    return 0;

  RIME_STRUCT (RimeContext, context);
  if (!self->api->get_context (self->session, &context))
    return 0;

  size_t count = (size_t) context.menu.num_candidates;
  self->api->free_context (&context);
  return count;
}

char *
myna_rime_get_candidate (MynaRime *self, size_t index)
{
  if (!self || !self->session)
    return NULL;

  RIME_STRUCT (RimeContext, context);
  if (!self->api->get_context (self->session, &context))
    return NULL;

  char *candidate = NULL;
  if (index < (size_t) context.menu.num_candidates) {
    RimeCandidate *item = &context.menu.candidates[index];
    candidate = dup_or_null (item->text);
  }

  self->api->free_context (&context);
  return candidate;
}

bool
myna_rime_is_composing (MynaRime *self)
{
  if (!self || !self->session)
    return false;

  RIME_STRUCT (RimeStatus, status);
  if (!self->api->get_status (self->session, &status))
    return false;

  bool composing = status.is_composing;
  self->api->free_status (&status);
  return composing;
}

void
myna_rime_string_free (char *value)
{
  g_free (value);
}
