/* Copyright (c) 2020 Endless OS Foundation LLC.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>. */

#pragma once

#include <gio/gio.h>
#include <glib.h>

#define DESKTOP_KEY_X_FLATPAK_PART_OF "X-Flatpak-Part-Of"

gboolean ensure_running_inside_flatpak();

gboolean mkdir_with_parents_exists_ok(GFile *dir, GError **error);

GFile *get_flextop_data_dir(GError **error);

gboolean delete_maybe_invalid_desktop_file(const char *path, GError **error);

typedef struct FlatpakInfo {
  char *app;
  char *branch;
  char *arch;
  char *app_commit;
  char *app_path;
} FlatpakInfo;

FlatpakInfo *flatpak_info_new();
gboolean flatpak_info_load(FlatpakInfo *info, GError **error);
char *flatpak_info_add_desktop_file_prefix(FlatpakInfo *info, const char *unprefixed);
void flatpak_info_free(FlatpakInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FlatpakInfo, flatpak_info_free)

typedef struct DataDir {
  GFile *root;
  GFile *applications;
  GFile *icons;
} DataDir;

DataDir *data_dir_new_for_root(GFile *root);
DataDir *data_dir_new_host(FlatpakInfo *info);
DataDir *data_dir_new_private();

gboolean data_dir_test_access(DataDir *dir);

void data_dir_free(DataDir *dir);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(DataDir, data_dir_free)
