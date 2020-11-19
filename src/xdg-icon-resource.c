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

#include "flextop-utils.h"

gboolean install(FlatpakInfo *info, DataDir *host, const char *icon_file,
                 const char *icon_name, int size, GError **error) {
  g_autofree char *size_dir = g_strdup_printf("%dx%d", size, size);
  g_autofree char *dest_dir =
      g_build_filename(g_file_peek_path(host->icons), "hicolor", size_dir, "apps", NULL);
  g_autoptr(GFile) dest_dir_file = g_file_new_for_path(dest_dir);
  if (!mkdir_with_parents_exists_ok(dest_dir_file, error)) {
    return FALSE;
  }

  g_autoptr(GFile) source_file = g_file_new_for_path(icon_file);
  g_autofree char *dest_filename = g_strdup_printf("%s.png", icon_name);
  g_autoptr(GFile) dest_file = g_file_get_child(dest_dir_file, dest_filename);
  if (!g_file_copy(source_file, dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL,
                   error)) {
    return FALSE;
  }

  return TRUE;
}

int main(int argc, char **argv) {
  g_set_prgname("xdg-icon-resource");

  g_autoptr(GError) error = NULL;

  if (argc != 8) {
    g_warning("usage: xdg-icon-resource install --mode user --size X file name");
    return 1;
  }

  char *size_str = argv[5];
  char *icon_file = argv[6];
  char *icon_name = argv[7];

  char *size_end;
  int size = g_strtod(size_str, &size_end);
  if (*size_end != '\0') {
    g_warning("Invalid size: %s", size_str);
    return 1;
  }

  if (!ensure_running_inside_flatpak()) {
    return 1;
  }

  g_autoptr(FlatpakInfo) info = flatpak_info_new();
  if (!flatpak_info_load(info, &error)) {
    g_warning("Failed to load flatpak info: %s", error->message);
    return 1;
  }

  g_autoptr(DataDir) host = data_dir_new_host(info);
  if (!data_dir_test_access(host)) {
    // Don't alert the user, because Chromium will run all the xdg-icon-resource
    // commands regardless of the individual exit statuses.
    g_warning("Warning: no host access");
    return 1;
  }

  if (!install(info, host, icon_file, icon_name, size, &error)) {
    g_warning("Failed to install icon file: %s", error->message);
    return 1;
  }

  return 0;
}
