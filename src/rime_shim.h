// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct MynaRime MynaRime;

MynaRime *myna_rime_create              (void);
void      myna_rime_destroy             (MynaRime *self);
bool      myna_rime_deploy_full         (void);
bool      myna_rime_process_key         (MynaRime *self, int key);
bool      myna_rime_select_candidate    (MynaRime *self, size_t index);
void      myna_rime_clear               (MynaRime *self);
char     *myna_rime_take_commit         (MynaRime *self);
char     *myna_rime_get_preedit         (MynaRime *self);
size_t    myna_rime_get_candidate_count (MynaRime *self);
char     *myna_rime_get_candidate       (MynaRime *self, size_t index);
bool      myna_rime_is_composing        (MynaRime *self);
void      myna_rime_string_free         (char *value);
