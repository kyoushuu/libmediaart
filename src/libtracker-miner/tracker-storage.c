/*
 * Copyright (C) 2008, Nokia (urho.konttori@nokia.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <string.h>

#include <gio/gio.h>

#include <libtracker-common/tracker-log.h>

#include "tracker-storage.h"
#include "tracker-utils.h"
#include "tracker-marshal.h"

#define TRACKER_STORAGE_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), TRACKER_TYPE_STORAGE, TrackerStoragePrivate))

typedef struct {
	GVolumeMonitor *volume_monitor;

	GNode *mounts;
	GHashTable *mounts_by_uuid;
} TrackerStoragePrivate;

typedef struct {
	gchar *mount_point;
	gchar *uuid;
	guint removable : 1;
} MountInfo;

typedef struct {
	const gchar *path;
	GNode *node;
} TraverseData;

typedef struct {
	GSList *roots;
	gboolean only_removable;
} GetRoots;

static void     tracker_storage_finalize (GObject        *object);
static gboolean mount_info_free          (GNode          *node,
                                          gpointer        user_data);
static void     mount_node_free          (GNode          *node);
static gboolean drives_setup             (TrackerStorage *storage);
static void     mount_added_cb           (GVolumeMonitor *monitor,
                                          GMount         *mount,
                                          gpointer        user_data);
static void     mount_removed_cb         (GVolumeMonitor *monitor,
                                          GMount         *mount,
                                          gpointer        user_data);
static void     volume_added_cb          (GVolumeMonitor *monitor,
                                          GVolume        *volume,
                                          gpointer        user_data);

enum {
	MOUNT_POINT_ADDED,
	MOUNT_POINT_REMOVED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

G_DEFINE_TYPE (TrackerStorage, tracker_storage, G_TYPE_OBJECT);

static void
tracker_storage_class_init (TrackerStorageClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS (klass);

	object_class->finalize     = tracker_storage_finalize;

	signals[MOUNT_POINT_ADDED] =
		g_signal_new ("mount-point-added",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL,
		              tracker_marshal_VOID__STRING_STRING,
		              G_TYPE_NONE,
		              2,
		              G_TYPE_STRING,
		              G_TYPE_STRING);

	signals[MOUNT_POINT_REMOVED] =
		g_signal_new ("mount-point-removed",
		              G_TYPE_FROM_CLASS (klass),
		              G_SIGNAL_RUN_LAST,
		              0,
		              NULL, NULL,
		              tracker_marshal_VOID__STRING_STRING,
		              G_TYPE_NONE,
		              2,
		              G_TYPE_STRING,
		              G_TYPE_STRING);

	g_type_class_add_private (object_class, sizeof (TrackerStoragePrivate));
}

static void
tracker_storage_init (TrackerStorage *storage)
{
	TrackerStoragePrivate *priv;

	g_message ("Initializing Storage...");

	priv = TRACKER_STORAGE_GET_PRIVATE (storage);

	priv->mounts = g_node_new (NULL);

	priv->mounts_by_uuid = g_hash_table_new_full (g_str_hash,
	                                              g_str_equal,
	                                              (GDestroyNotify) g_free,
	                                              NULL);

	priv->volume_monitor = g_volume_monitor_get ();

	/* Volume and property notification callbacks */
	g_signal_connect_object (priv->volume_monitor, "mount_removed",
	                         G_CALLBACK (mount_removed_cb), storage, 0);
	g_signal_connect_object (priv->volume_monitor, "mount_pre_unmount",
	                         G_CALLBACK (mount_removed_cb), storage, 0);
	g_signal_connect_object (priv->volume_monitor, "mount_added",
	                         G_CALLBACK (mount_added_cb), storage, 0);
	g_signal_connect_object (priv->volume_monitor, "volume_added",
	                         G_CALLBACK (volume_added_cb), storage, 0);

	g_message ("Drive/Volume monitors set up for to watch for added, removed and pre-unmounts...");

	/* Get all devices which are mountable and set them up */
	if (!drives_setup (storage)) {
		return;
	}
}

static void
tracker_storage_finalize (GObject *object)
{
	TrackerStoragePrivate *priv;

	priv = TRACKER_STORAGE_GET_PRIVATE (object);

	if (priv->mounts_by_uuid) {
		g_hash_table_unref (priv->mounts_by_uuid);
	}

	if (priv->mounts) {
		mount_node_free (priv->mounts);
	}

	if (priv->volume_monitor) {
		g_object_unref (priv->volume_monitor);
	}

	(G_OBJECT_CLASS (tracker_storage_parent_class)->finalize) (object);
}

static void
mount_node_free (GNode *node)
{
	g_node_traverse (node,
	                 G_POST_ORDER,
	                 G_TRAVERSE_ALL,
	                 -1,
	                 mount_info_free,
	                 NULL);

	g_node_destroy (node);
}

static gboolean
mount_node_traverse_func (GNode    *node,
                          gpointer  user_data)
{
	TraverseData *data;
	MountInfo *info;

	if (!node->data) {
		/* Root node */
		return FALSE;
	}

	data = user_data;
	info = node->data;

	if (g_str_has_prefix (data->path, info->mount_point)) {
		data->node = node;
		return TRUE;
	}

	return FALSE;
}

static GNode *
mount_node_find (GNode       *root,
                 const gchar *path)
{
	TraverseData data = { path, NULL };

	g_node_traverse (root,
	                 G_POST_ORDER,
	                 G_TRAVERSE_ALL,
	                 -1,
	                 mount_node_traverse_func,
	                 &data);

	return data.node;
}

static gboolean
mount_info_free (GNode    *node,
                 gpointer  user_data)
{
	MountInfo *info;

	info = node->data;

	if (info) {
		g_free (info->mount_point);
		g_free (info->uuid);

		g_slice_free (MountInfo, info);
	}

	return FALSE;
}

static MountInfo *
mount_info_find (GNode       *root,
                 const gchar *path)
{
	GNode *node;

	node = mount_node_find (root, path);
	return (node) ? node->data : NULL;
}

static gchar *
mount_point_normalize (const gchar *mount_point)
{
	gchar *mp;

	/* Normalize all mount points to have a / at the end */
	if (g_str_has_suffix (mount_point, G_DIR_SEPARATOR_S)) {
		mp = g_strdup (mount_point);
	} else {
		mp = g_strconcat (mount_point, G_DIR_SEPARATOR_S, NULL);
	}

	return mp;
}

static GNode *
mount_add_hierarchy (GNode       *root,
                     const gchar *uuid,
                     const gchar *mount_point,
                     gboolean     removable)
{
	MountInfo *info;
	GNode *node;
	gchar *mp;

	mp = mount_point_normalize (mount_point);
	node = mount_node_find (root, mp);

	if (!node) {
		node = root;
	}

	info = g_slice_new (MountInfo);
	info->mount_point = mp;
	info->uuid = g_strdup (uuid);
	info->removable = removable;

	return g_node_append_data (node, info);
}

static void
mount_add (TrackerStorage *storage,
           const gchar    *uuid,
           const gchar    *mount_point,
           gboolean        removable_device)
{
	TrackerStoragePrivate *priv;
	GNode *node;

	priv = TRACKER_STORAGE_GET_PRIVATE (storage);

	node = mount_add_hierarchy (priv->mounts, uuid, mount_point, removable_device);
	g_hash_table_insert (priv->mounts_by_uuid, g_strdup (uuid), node);

	g_signal_emit (storage, signals[MOUNT_POINT_ADDED], 0, uuid, mount_point, NULL);
}

static void
volume_add (TrackerStorage *storage,
            GVolume        *volume,
            gboolean        initialization)
{
	TrackerStoragePrivate *priv;
	GDrive *drive;
	GMount *mount;
	gchar *str;
	gboolean is_mounted;
	gchar *uuid;
	gchar *mount_point;
	gchar *device_file;

	drive = g_volume_get_drive (volume);

	if (!initialization) {
		g_debug ("Drive:'%s' added 1 volume:",
		         g_drive_get_name (drive));
	}

	str = g_volume_get_name (volume);
	g_debug ("  Volume:'%s' found", str);
	g_free (str);
		
	if (!g_volume_should_automount (volume) ||
	    !g_volume_can_mount (volume)) {
		g_debug ("    Ignoring, volume can not be automatically mounted or mounted at all");
		return;
	}

	device_file = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
	g_debug ("    Device file  : %s", device_file);
		
	mount = g_volume_get_mount (volume); 

	if (mount) {
		GFile *file;
		
		file = g_mount_get_root (mount);
		
		mount_point = g_file_get_path (file);
		g_debug ("    Mount point  : %s", mount_point);
		
		g_object_unref (file);
		g_object_unref (mount);
		
		is_mounted = TRUE;
	} else {
		mount_point = NULL;
		is_mounted = FALSE;
	}
	
	uuid = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_UUID);
	g_debug ("    UUID         : %s", uuid);
	
	g_debug ("    Mounted      : %s", is_mounted ? "yes" : "no");
	
	priv = TRACKER_STORAGE_GET_PRIVATE (storage);

	if (mount_point && !g_hash_table_lookup (priv->mounts_by_uuid, uuid)) {
		mount_add (storage, uuid, mount_point, TRUE);
	}
	
	g_free (uuid);
	g_free (mount_point);
	g_free (device_file);
}

static gboolean
drives_setup (TrackerStorage *storage)
{
	TrackerStoragePrivate *priv;
	GList *drives, *ld;

	priv = TRACKER_STORAGE_GET_PRIVATE (storage);

	drives = g_volume_monitor_get_connected_drives (priv->volume_monitor);

	if (g_list_length (drives) < 1) {
		g_message ("No drives found to iterate");
		return TRUE;
	}

	for (ld = drives; ld; ld = ld->next) {
		GDrive *drive;
		GList *volumes, *lv;
		guint n_volumes;

		drive = ld->data;

		if (!drive) {
			continue;
		}
		
		volumes = g_drive_get_volumes (drive);
		n_volumes = g_list_length (volumes);

		g_debug ("Drive:'%s' found with %d %s:",
		         g_drive_get_name (drive),
		         n_volumes,
		         n_volumes == 1 ? "volume" : "volumes");

		for (lv = volumes; lv; lv = lv->next) {
			volume_add (storage, lv->data, TRUE);
			g_object_unref (lv->data);
		}

		g_list_free (volumes);
		g_object_unref (ld->data);
	}

	g_list_free (drives);

	return TRUE;
}

static void
mount_added_cb (GVolumeMonitor *monitor,
                GMount         *mount,
                gpointer        user_data)
{
	TrackerStorage *storage;
	GVolume *volume;
	GFile *file;
	gchar *device_file;
	gchar *uuid;
	gchar *mount_point;
	gboolean removable_device = TRUE;

	storage = user_data;

	volume = g_mount_get_volume (mount);
	device_file = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
	uuid = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_UUID);
	file = g_mount_get_root (mount);
	mount_point = g_file_get_path (file);

	/* NOTE: We only deal with removable devices */
	removable_device = TRUE;

	g_message ("Device:'%s', UUID:'%s' now mounted on:'%s'",
	           device_file,
	           uuid,
	           mount_point);

	/* We don't have a UUID for CDROMs */
	if (uuid) {
		g_message ("  Being added as a tracker resource to index!");
		mount_add (storage, uuid, mount_point, removable_device);
	} else {
		g_message ("  Being ignored because we have no UUID");
	}

	g_free (mount_point);
	g_object_unref (file);
	g_free (uuid);
	g_free (device_file);
	g_object_unref (volume);
}

static void
mount_removed_cb (GVolumeMonitor *monitor,
                  GMount         *mount,
                  gpointer        user_data)
{
	TrackerStorage *storage;
	TrackerStoragePrivate *priv;
	MountInfo *info;
	GNode *node;
	GFile *file;
	gchar *name;
	gchar *mount_point;
	gchar *mp;

	storage = user_data;
	priv = TRACKER_STORAGE_GET_PRIVATE (storage);

	file = g_mount_get_root (mount);
	mount_point = g_file_get_path (file);
	name = g_mount_get_name (mount);

	mp = mount_point_normalize (mount_point);
	node = mount_node_find (priv->mounts, mp);
	g_free (mp);

	if (node) {
		info = node->data;

		g_message ("Mount:'%s' with UUID:'%s' now unmounted from:'%s'",
		           name,
		           info->uuid,
		           mount_point);

		g_signal_emit (storage, signals[MOUNT_POINT_REMOVED], 0, info->uuid, mount_point, NULL);
		
		g_hash_table_remove (priv->mounts_by_uuid, info->uuid);
		mount_node_free (node);
	} else {
		g_message ("Mount:'%s' now unmounted from:'%s' (was not tracked)",
		           name,
		           mount_point);
	}

	g_free (name);
	g_free (mount_point);
	g_object_unref (file);
}

static void
volume_added_cb (GVolumeMonitor *monitor,
                 GVolume        *volume,
                 gpointer        user_data)
{
	volume_add (user_data, volume, FALSE);
}

/**
 * tracker_storage_new:
 *
 * Creates a new instance of #TrackerStorage.
 *
 * Returns: The newly created #TrackerStorage.
 **/
TrackerStorage *
tracker_storage_new (void)
{
	return g_object_new (TRACKER_TYPE_STORAGE, NULL);
}

static void
get_mount_point_by_uuid_foreach (gpointer key,
                                 gpointer value,
                                 gpointer user_data)
{
	GetRoots *gr;
	const gchar *uuid;
	GNode *node;
	MountInfo *info;

	gr = user_data;
	uuid = key;
	node = value;
	info = node->data;

	if (!gr->only_removable || info->removable) {
		gchar *normalized_mount_point;
		gint len;

		normalized_mount_point = g_strdup (info->mount_point);
		len = strlen (normalized_mount_point);

		/* Don't include trailing slashes */
		if (len > 2 && normalized_mount_point[len - 1] == G_DIR_SEPARATOR) {
			normalized_mount_point[len - 1] = '\0';
		}

		gr->roots = g_slist_prepend (gr->roots, normalized_mount_point);
	}
}

/**
 * tracker_storage_get_removable_device_roots:
 * @storage: A #TrackerStorage
 *
 * Returns a #GSList of strings containing the root directories for
 * removable devices.
 *
 * Each element must be freed using g_free() and the list itself
 * through g_slist_free().
 *
 * Returns: The list of root directories.
 **/
GSList *
tracker_storage_get_removable_device_roots (TrackerStorage *storage)
{
	TrackerStoragePrivate *priv;
	GetRoots gr;

	g_return_val_if_fail (TRACKER_IS_STORAGE (storage), NULL);

	priv = TRACKER_STORAGE_GET_PRIVATE (storage);

	gr.roots = NULL;
	gr.only_removable = TRUE;

	g_hash_table_foreach (priv->mounts_by_uuid,
	                      get_mount_point_by_uuid_foreach,
	                      &gr);

	return g_slist_reverse (gr.roots);
}

/**
 * tracker_storage_get_removable_device_uuids:
 * @storage: A #TrackerStorage
 *
 * Returns a #GSList of strings containing the UUID for removable devices.
 * Each element is owned by the #GHashTable internally, the list
 * itself through should be freed using g_slist_free().
 *
 * Returns: The list of UUIDs.
 **/
GSList *
tracker_storage_get_removable_device_uuids (TrackerStorage *storage)
{
	TrackerStoragePrivate *priv;
	GHashTableIter iter;
	gpointer key, value;
	GSList *uuids;

	g_return_val_if_fail (TRACKER_IS_STORAGE (storage), NULL);

	priv = TRACKER_STORAGE_GET_PRIVATE (storage);

	uuids = NULL;

	g_hash_table_iter_init (&iter, priv->mounts_by_uuid);

	while (g_hash_table_iter_next (&iter, &key, &value)) {
		const gchar *uuid;
		GNode *node;
		MountInfo *info;

		uuid = key;
		node = value;
		info = node->data;

		if (info->removable) {
			uuids = g_slist_prepend (uuids, g_strdup (uuid));
		}
	}

	return g_slist_reverse (uuids);
}

/**
 * tracker_storage_get_mount_point_for_uuid:
 * @storage: A #TrackerStorage
 * @uuid: A string pointer to the UUID for the %GVolume.
 *
 * Returns: The mount point for @uuid, this should not be freed.
 **/
const gchar *
tracker_storage_get_mount_point_for_uuid (TrackerStorage *storage,
                                          const gchar    *uuid)
{
	TrackerStoragePrivate *priv;
	GNode *node;
	MountInfo *info;

	g_return_val_if_fail (TRACKER_IS_STORAGE (storage), NULL);
	g_return_val_if_fail (uuid != NULL, NULL);

	priv = TRACKER_STORAGE_GET_PRIVATE (storage);

	node = g_hash_table_lookup (priv->mounts_by_uuid, uuid);

	if (!node) {
		return NULL;
	}

	info = node->data;

	return info->mount_point;
}

/**
 * tracker_storage_get_uuid_for_file:
 * @storage: A #TrackerStorage
 * @file: a file
 *
 * Returns the UUID of the removable device for @file
 *
 * Returns: Returns the UUID of the removable device for @file, this
 * should not be freed.
 **/
const gchar *
tracker_storage_get_uuid_for_file (TrackerStorage *storage,
                                   GFile          *file)
{
	TrackerStoragePrivate *priv;
	gchar *path;
	MountInfo *info;

	g_return_val_if_fail (TRACKER_IS_STORAGE (storage), FALSE);

	path = g_file_get_path (file);

	if (!path) {
		return NULL;
	}

	/* Normalize all paths to have a / at the end */
	if (!g_str_has_suffix (path, G_DIR_SEPARATOR_S)) {
		gchar *norm_path;

		norm_path = g_strconcat (path, G_DIR_SEPARATOR_S, NULL);
		g_free (path);
		path = norm_path;
	}

	priv = TRACKER_STORAGE_GET_PRIVATE (storage);

	info = mount_info_find (priv->mounts, path);

	if (!info) {
		g_free (path);
		return NULL;
	}

	/* g_debug ("Mount for path '%s' is '%s' (UUID:'%s')", */
	/*          path, info->mount_point, info->uuid); */

	g_free (path);

	return info->uuid;
}
