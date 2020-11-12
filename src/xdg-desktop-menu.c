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

#include <gtk/gtk.h>

gboolean ensure_host_access(DataDir *host) {
  if (!data_dir_test_access(host)) {
    gtk_init(0, NULL);
    GtkWidget *dialog = gtk_message_dialog_new(
        NULL, 0, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
        "This Flatpak does not have write access to ~/.local/share/applications"
        " and ~/.local/share/icons, so it cannot install or uninstall PWAs.\n\n"
        "Once you grant access to those two directories (Flatseal is the easiest"
        " method), you can attempt to re-create the shortcuts from"
        " chrome://apps.");

    gtk_dialog_run(GTK_DIALOG(dialog));

    return FALSE;
  }

  return TRUE;
}

void edit_icon_key(GKeyFile *key_file, const char *section, FlatpakInfo *info) {
  g_autofree char *icon =
      g_key_file_get_string(key_file, section, G_KEY_FILE_DESKTOP_KEY_ICON, NULL);
  if (icon != NULL) {
    g_autofree char *prefixed_icon = add_app_icon_prefix(info, icon);
    g_key_file_set_string(key_file, section, G_KEY_FILE_DESKTOP_KEY_ICON, prefixed_icon);
  }
}

gboolean edit_exec_key(GKeyFile *key_file, const char *section, FlatpakInfo *info,
                       GError **error) {
  int argc;
  g_auto(GStrv) argv = NULL;

  g_autofree char *exec =
      g_key_file_get_string(key_file, section, G_KEY_FILE_DESKTOP_KEY_EXEC, NULL);
  if (exec == NULL) {
    g_warning("Missing Exec key in %s", section);
    return TRUE;
  }

  if (!g_shell_parse_argv(exec, &argc, &argv, error)) {
    g_prefix_error(error, "Getting command of %s: ", section);
    return FALSE;
  }

  if (argc < 1) {
    g_warning("Empty Exec key in %s", section);
    return TRUE;
  }

  g_autoptr(GPtrArray) new_argv = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(new_argv, g_strdup("flatpak"));
  g_ptr_array_add(new_argv, g_strdup("run"));
  g_ptr_array_add(new_argv, g_strdup_printf("--branch=%s", info->branch));
  g_ptr_array_add(new_argv, g_strdup_printf("--arch=%s", info->arch));
  g_ptr_array_add(new_argv, g_strdup_printf("--command=%s", argv[0]));
  g_ptr_array_add(new_argv, g_strdup(info->app));

  for (int i = 1; i < argc; i++) {
    g_ptr_array_add(new_argv, g_strdup(argv[i]));
  }

  for (int i = 0; i < new_argv->len; i++) {
    g_autofree char *unquoted = g_ptr_array_index(new_argv, i);
    g_ptr_array_index(new_argv, i) = g_shell_quote(unquoted);
  }

  g_ptr_array_add(new_argv, NULL);
  g_autofree char *command = g_strjoinv(" ", (char **)new_argv->pdata);
  g_key_file_set_string(key_file, section, G_KEY_FILE_DESKTOP_KEY_EXEC, command);

  return TRUE;
}

gboolean edit_keys(GKeyFile *key_file, const char *section, FlatpakInfo *info,
                   GError **error) {
  edit_icon_key(key_file, section, info);
  return edit_exec_key(key_file, section, info, error);
}

gboolean install(GPtrArray *paths, FlatpakInfo *info, DataDir *host, GError **error) {
  for (int i = 0; i < paths->len; i++) {
    const char *path = g_ptr_array_index(paths, i);

    g_autoptr(GKeyFile) key_file = g_key_file_new();
    if (!g_key_file_load_from_file(
            key_file, path, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
            error)) {
      g_prefix_error(error, "Loading %s: ", path);
      return FALSE;
    }

    if (!edit_keys(key_file, G_KEY_FILE_DESKTOP_GROUP, info, error)) {
      return FALSE;
    }

    g_auto(GStrv) actions = g_key_file_get_string_list(
        key_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_ACTIONS, NULL, NULL);
    if (actions) {
      for (char **action = actions; *action; action++) {
        g_autofree char *section = g_strdup_printf("Desktop Action %s", *action);
        if (!edit_keys(key_file, section, info, error)) {
          return FALSE;
        }
      }
    }

    g_autofree char *filename = g_path_get_basename(path);
    g_autofree char *dest =
        g_build_filename(g_file_peek_path(host->applications), filename, NULL);
    if (!g_key_file_save_to_file(key_file, dest, error)) {
      return FALSE;
    }
  }

  return TRUE;
}

GPtrArray *find_all_files_for_app_icon(GFile *icons, const char *icon) {
  g_autoptr(GPtrArray) result = g_ptr_array_new_with_free_func(g_object_unref);
  g_autoptr(GError) error = NULL;

  // XXX: We're tied to .png icons for now.
  g_autofree char *icon_filename = g_strdup_printf("%s.png", icon);

  g_autoptr(GFile) hicolor = g_file_get_child(icons, "hicolor");
  g_autoptr(GFileEnumerator) size_dirs =
      g_file_enumerate_children(hicolor, "standard::name,standard::type",
                                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
  if (size_dirs == NULL) {
    g_warning("Failed to iterate over icon size dirs: %s", error->message);
  } else {
    GFileInfo *size_dir_info = NULL;
    GFile *size_dir_file = NULL;
    for (;;) {
      if (!g_file_enumerator_iterate(size_dirs, &size_dir_info, &size_dir_file, NULL,
                                     &error)) {
        g_warning("Failed to continue iteration over icon size dirs: %s", error->message);
        break;
      }

      if (size_dir_info == NULL) {
        break;
      }
      g_warn_if_fail(size_dir_file != NULL);

      if (g_file_info_get_file_type(size_dir_info) != G_FILE_TYPE_DIRECTORY) {
        continue;
      }

      g_autoptr(GFile) apps = g_file_get_child(size_dir_file, "apps");
      g_autoptr(GFile) icon_file = g_file_get_child(apps, icon_filename);
      if (g_file_query_exists(icon_file, NULL)) {
        g_ptr_array_add(result, g_steal_pointer(&icon_file));
      }
    }
  }

  return g_steal_pointer(&result);
}

gboolean uninstall(GPtrArray *filenames, FlatpakInfo *info, DataDir *host,
                   GError **error) {
  for (int i = 0; i < filenames->len; i++) {
    const char *filename = g_ptr_array_index(filenames, i);

    g_autoptr(GFile) file = g_file_get_child(host->applications, filename);
    if (!g_file_query_exists(file, NULL)) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                  "Desktop file %s does not exist", filename);
      return FALSE;
    }

    g_autoptr(GKeyFile) key_file = g_key_file_new();
    if (!g_key_file_load_from_file(key_file, g_file_peek_path(file), G_KEY_FILE_NONE,
                                   error)) {
      return FALSE;
    }

    char *icon_name = g_key_file_get_string(key_file, G_KEY_FILE_DESKTOP_GROUP,
                                            G_KEY_FILE_DESKTOP_KEY_ICON, NULL);
    if (icon_name != NULL) {
      g_autoptr(GPtrArray) icons = find_all_files_for_app_icon(host->icons, icon_name);
      for (int j = 0; j < icons->len; j++) {
        GFile *file = g_ptr_array_index(icons, i);
        g_autoptr(GError) local_error = NULL;
        if (!g_file_delete(file, NULL, &local_error)) {
          if (!g_error_matches(local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
            g_warning("Unexpected error removing icon %s: %s", g_file_peek_path(file),
                      local_error->message);
          }
        }
      }
    }

    if (!g_file_delete(file, NULL, error)) {
      return FALSE;
    }
  }

  return TRUE;
}

int main(int argc, char **argv) {
  g_set_prgname("xdg-desktop-menu");

  g_autoptr(GError) error = NULL;

  if (argc < 4) {
    g_warning("usage: xdg-desktop-menu install|uninstall --mode user app.desktop...");
    return 1;
  }

  if (!ensure_running_inside_flatpak()) {
    return 1;
  }

  g_autoptr(FlatpakInfo) info = flatpak_info_new();
  if (!flatpak_info_load(info, &error)) {
    g_warning("Failed to load flatpak app info: %s", error->message);
    return 1;
  }

  DataDir *host = data_dir_new_host(info);
  if (!ensure_host_access(host)) {
    return 1;
  }

  g_autoptr(GPtrArray) args = g_ptr_array_new();
  for (int i = 4; i < argc; i++) {
    if (g_str_has_suffix(argv[i], ".desktop")) {
      g_ptr_array_add(args, argv[i]);
    }
  }

  gboolean success = FALSE;
  if (strcmp(argv[1], "install") == 0) {
    success = install(args, info, host, &error);
  } else if (strcmp(argv[1], "uninstall") == 0) {
    success = uninstall(args, info, host, &error);
  } else {
    g_warning("Unknown command: %s", argv[1]);
    return 1;
  }

  if (!success) {
    g_warning("Failed to process file: %s", error->message);
    return 1;
  }

  return 0;
}
