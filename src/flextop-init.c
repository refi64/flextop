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

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <sys/file.h>

typedef int LockFd;
G_DEFINE_AUTO_CLEANUP_FREE_FUNC(LockFd, close, -1)

LockFd acquire_lock(FlatpakInfo *info, GError **error) {
  g_autofree char *lock_filename =
      g_build_filename(g_get_user_runtime_dir(), "app", info->app, ".flextop-lock", NULL);
  int fd = open(lock_filename, O_CREAT | O_RDWR, 0600);
  if (fd == -1) {
    int err = errno;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err), "Failed to open lock: %s",
                strerror(err));
    return -1;
  }

  if (flock(fd, LOCK_EX) == -1) {
    int err = errno;
    close(fd);

    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(err), "Failed to set lock: %s",
                strerror(err));
    return -1;
  }

  return fd;
}

gboolean atomic_relink(GFile *link, const char *target, GError **error) {
  g_autofree char *temp_path = g_strdup_printf("%s.tmp", g_file_peek_path(link));
  g_autoptr(GFile) temp = g_file_new_for_path(temp_path);

  if (!g_file_delete(temp, NULL, error)) {
    if (!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
      g_prefix_error(error, "Deleting old symlink: ");
      return FALSE;
    }

    g_clear_error(error);
  }

  if (!g_file_make_symbolic_link(temp, target, NULL, error)) {
    g_prefix_error(error, "Symlink %s as %s: ", target, temp_path);
    return FALSE;
  }

  if (!g_file_move(temp, link,
                   G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE |
                       G_FILE_COPY_OVERWRITE,
                   NULL, NULL, NULL, error)) {
    g_prefix_error(error, "Overwriting symlink: ");
    return FALSE;
  }

  return TRUE;
}

GFile *get_sibling_file(GFile *file, const char *sibling_name) {
  g_autoptr(GFile) parent = g_file_get_parent(file);
  return g_file_get_child(parent, sibling_name);
}

gboolean migrate_prefix_desktop_file(FlatpakInfo *info, GFile *file, GFileInfo *file_info,
                                     GError **error) {
  g_autofree char *prefix = g_strdup_printf("%s.", info->app);
  if (g_str_has_prefix(g_file_info_get_name(file_info), prefix)) {
    // Already migrated.
    return TRUE;
  }

  g_autoptr(GKeyFile) key_file = g_key_file_new();
  if (!g_key_file_load_from_file(key_file, g_file_peek_path(file), G_KEY_FILE_NONE,
                                 error)) {
    return FALSE;
  }

  g_autofree char *part_of = g_key_file_get_string(key_file, G_KEY_FILE_DESKTOP_GROUP,
                                                   DESKTOP_KEY_X_FLATPAK_PART_OF, NULL);
  if (g_strcmp0(part_of, info->app) != 0) {
    // Not our file to worry about.
    return TRUE;
  }

  g_debug("Migrate file: %s", g_file_peek_path(file));

  g_autofree char *prefixed_basename =
      flatpak_info_add_desktop_file_prefix(info, g_file_info_get_name(file_info));
  g_autoptr(GFile) prefixed_file = get_sibling_file(file, prefixed_basename);

  if (!g_file_move(file, prefixed_file, G_FILE_COPY_NO_FALLBACK_FOR_MOVE, NULL, NULL,
                   NULL, error)) {
    g_prefix_error(error, "Migrating desktop file %s", g_file_peek_path(file));
    return FALSE;
  }

  return TRUE;
}

gboolean migrate_prefix_all_desktop_files(FlatpakInfo *info, DataDir *priv,
                                          GError **error) {
  g_autoptr(GFile) flextop_data = get_flextop_data_dir(error);
  if (flextop_data == NULL) {
    return FALSE;
  }

  g_autoptr(GFile) migration_stamp = g_file_get_child(flextop_data, "prefixed-app-ids");
  if (g_file_query_exists(migration_stamp, NULL)) {
    // Already migrated.
    return TRUE;
  }

  g_autoptr(GFileEnumerator) enumerator = g_file_enumerate_children(
      priv->applications,
      G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
      G_FILE_QUERY_INFO_NONE, NULL, error);
  if (enumerator == NULL) {
    if (g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
      g_clear_error(error);
    } else {
      g_prefix_error(error, "Enumerating files to migrate");
      return FALSE;
    }
  } else {
    for (;;) {
      GFileInfo *child_info = NULL;
      GFile *child = NULL;

      if (!g_file_enumerator_iterate(enumerator, &child_info, &child, NULL, error)) {
        return FALSE;
      } else if (child_info == NULL) {
        // No more files.
        break;
      }

      if (g_file_info_get_file_type(child_info) == G_FILE_TYPE_REGULAR &&
          g_str_has_suffix(g_file_info_get_name(child_info), ".desktop") &&
          !migrate_prefix_desktop_file(info, child, child_info, error)) {
        return FALSE;
      }
    }
  }

  if (!g_file_set_contents(g_file_peek_path(migration_stamp), "", 0, error)) {
    g_prefix_error(error, "Setting migration stamp");
    return FALSE;
  }

  return TRUE;
}

gboolean setup_applications_folder(FlatpakInfo *info, DataDir *host, DataDir *priv,
                                   GError **error) {
  if (!mkdir_with_parents_exists_ok(host->applications, error)) {
    return FALSE;
  }

  GFileInfo *applications_info =
      g_file_query_info(priv->applications, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
  if (!applications_info) {
    if (!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
      g_prefix_error(error, "query %s", g_file_peek_path(priv->applications));
      return FALSE;
    }

    g_clear_error(error);
  }

  gboolean should_migrate = applications_info != NULL;

  // If the applications path exists as a directory already, then someone has
  // tried installing PWAs or creating shortcuts without flextop. For safety,
  // it's easiest to just rename it to the first other path we can.
  if (applications_info &&
      g_file_info_get_file_type(applications_info) == G_FILE_TYPE_DIRECTORY) {
    for (int i = 0;; i++) {
      g_autofree char *new_name =
          g_strdup_printf("%s.%d", g_file_peek_path(priv->applications), i);
      GFile *new_file = g_file_new_for_path(new_name);
      if (!g_file_move(priv->applications, new_file,
                       G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE,
                       NULL, NULL, NULL, error)) {
        if (g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
          // Just try the next name.
          g_clear_error(error);
          continue;
        }

        g_prefix_error(error, "Rename %s -> %s", g_file_peek_path(priv->applications),
                       g_file_peek_path(new_file));
        return FALSE;
      }

      break;
    }
  }

  if (!atomic_relink(priv->applications, g_file_peek_path(host->applications), error)) {
    return FALSE;
  }

  if (should_migrate && !migrate_prefix_all_desktop_files(info, priv, error)) {
    return FALSE;
  }

  return TRUE;
}

gboolean delete_invalid_desktop_files(GError **error) {
  const char *desktop_dir = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
  if (desktop_dir == NULL) {
    return TRUE;
  }

  g_autoptr(GFile) desktop_dir_file = g_file_new_for_path(desktop_dir);
  g_autoptr(GFileEnumerator) enumerator = g_file_enumerate_children(
      desktop_dir_file, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
      G_FILE_QUERY_INFO_NONE, NULL, error);
  if (enumerator == NULL) {
    if (g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
      g_clear_error(error);
      return TRUE;
    } else {
      g_prefix_error(error, "Enumerating files to migrate");
      return FALSE;
    }
  }

  for (;;) {
    GFileInfo *child_info = NULL;
    GFile *child = NULL;

    if (!g_file_enumerator_iterate(enumerator, &child_info, &child, NULL, error)) {
      return FALSE;
    } else if (child_info == NULL) {
      // No more files.
      break;
    }

    if (g_file_info_get_file_type(child_info) == G_FILE_TYPE_REGULAR &&
        g_str_has_suffix(g_file_info_get_name(child_info), ".desktop")) {
      g_autofree char *path =
          g_build_filename(desktop_dir, g_file_info_get_name(child_info), NULL);
      g_autoptr(GError) local_error = NULL;
      if (!delete_maybe_invalid_desktop_file(path, &local_error)) {
        g_warning("Failed to check desktop file: %s", local_error->message);
      }
    }
  }

  return TRUE;
}

int main() {
  g_set_prgname("flextop-init");

  g_autoptr(GError) error = NULL;

  if (!ensure_running_inside_flatpak()) {
    return 1;
  }

  g_autoptr(FlatpakInfo) info = flatpak_info_new();
  if (!flatpak_info_load(info, &error)) {
    g_warning("Failed to load flatpak info: %s", error->message);
    return 1;
  }

  g_auto(LockFd) lock = acquire_lock(info, &error);
  if (lock == -1) {
    g_warning("%s", error->message);
    return 1;
  }

  g_autoptr(DataDir) host = data_dir_new_host(info);
  g_autoptr(DataDir) priv = data_dir_new_private();

  if (!setup_applications_folder(info, host, priv, &error)) {
    g_warning("Failed to set up applications folder: %s", error->message);
    return 1;
  }

  if (!delete_invalid_desktop_files(&error)) {
    g_warning("Failed to delete invalid desktop files: %s", error->message);
    return 1;
  }

  return 0;
}
