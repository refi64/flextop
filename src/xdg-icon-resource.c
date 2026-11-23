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

void usage(int argc, char **argv) {
  g_auto(GStrv) quoted_argv = g_new0(char *, argc + 1);
  for (int i = 0; i < argc; i++) {
    quoted_argv[i] = g_shell_quote(argv[i]);
  }

  g_autofree char *invocation = g_strjoinv(" ", quoted_argv);
  g_warning("Unsupported invocation of xdg-icon-resource: %s", invocation);
  g_warning("usage: xdg-icon-resource install --mode user --size X file name");
  g_warning("       xdg-icon-resource uninstall --mode user --size X name");
}

GFile *get_icon_file(DataDir *host, const char *icon_name, int size) {
  g_autofree char *size_dir = g_strdup_printf("%dx%d", size, size);
  g_autofree char *icon_filename = g_str_has_suffix(icon_name, ".png")
                                       ? g_strdup(icon_name)
                                       : g_strdup_printf("%s.png", icon_name);
  g_autofree char *icon_path = g_build_filename(g_file_peek_path(host->icons), "hicolor",
                                                size_dir, "apps", icon_filename, NULL);
  return g_file_new_for_path(icon_path);
}

gboolean install(DataDir *host, const char *icon_file, const char *icon_name, int size,
                 GError **error) {
  g_autoptr(GFile) dest_file = get_icon_file(host, icon_name, size);
  g_autoptr(GFile) dest_parent = g_file_get_parent(dest_file);
  if (!mkdir_with_parents_exists_ok(dest_parent, error)) {
    return FALSE;
  }

  g_autoptr(GFile) source_file = g_file_new_for_path(icon_file);
  if (!g_file_copy(source_file, dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL,
                   error)) {
    return FALSE;
  }

  return TRUE;
}

gboolean uninstall(DataDir *host, const char *icon_name, int size, GError **error) {
  g_autoptr(GFile) dest_file = get_icon_file(host, icon_name, size);
  g_autoptr(GError) local_error = NULL;
  if (!g_file_delete(dest_file, NULL, &local_error)) {
    if (!g_error_matches(local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
      g_propagate_error(error, g_steal_pointer(&local_error));
      return FALSE;
    }
  }

  return TRUE;
}

int main(int argc, char **argv) {
  g_set_prgname("xdg-icon-resource");

  g_autoptr(GError) error = NULL;

  if (argc < 6 || !g_str_equal(argv[2], "--mode") || !g_str_equal(argv[3], "user") ||
      !g_str_equal(argv[4], "--size")) {
    usage(argc, argv);
    return 1;
  }

  typedef enum {
    COMMAND_INSTALL,
    COMMAND_UNINSTALL,
  } Command;

  char *command_s = argv[1];
  Command command;
  char *icon_file = NULL;
  char *icon_name = NULL;
  if (g_str_equal(command_s, "install") && argc == 8) {
    command = COMMAND_INSTALL;
    icon_file = argv[6];
    icon_name = argv[7];
  } else if (g_str_equal(command_s, "uninstall") && argc == 7) {
    command = COMMAND_UNINSTALL;
    icon_name = argv[6];
  } else {
    usage(argc, argv);
    return 1;
  }

  char *size_str = argv[5];
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

  gboolean success = FALSE;
  switch (command) {
  case COMMAND_INSTALL:
    success = install(host, icon_file, icon_name, size, &error);
    break;
  case COMMAND_UNINSTALL:
    success = uninstall(host, icon_name, size, &error);
    break;
  default:
    g_assert_not_reached();
  }

  if (!success) {
    g_warning("Failed to %s icon file: %s", command_s, error->message);
    return 1;
  }

  return 0;
}
