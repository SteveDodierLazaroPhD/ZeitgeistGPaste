/*
 *      This file is part of GPaste.
 *
 *      Copyright 2011-2014 Marc-Antoine Perennou <Marc-Antoine@Perennou.com>
 *
 *      GPaste is free software: you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation, either version 3 of the License, or
 *      (at your option) any later version.
 *
 *      GPaste is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with GPaste.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "gpaste-clipboard-private.h"

#include <gpaste-image-item.h>
#include <gpaste-password-item.h>
#include <gpaste-uris-item.h>

#include <string.h>
#include <zeitgeist.h>
#include <linux/limits.h>
#define ZEITGEIST_ZG_CLIPBOARD_COPY_EVENT "http://www.zeitgeist-project.com/ontologies/2010/01/27/zg#ClipboardCopy"

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

struct _GPasteClipboardPrivate
{
    GdkAtom         target;
    GtkClipboard   *real;
    GPasteSettings *settings;
    gchar          *text;
    gchar          *image_checksum;

    gulong          owner_change_signal;
};

G_DEFINE_TYPE_WITH_PRIVATE (GPasteClipboard, g_paste_clipboard, G_TYPE_OBJECT)

enum
{
    OWNER_CHANGE,

    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
g_paste_clipboard_bootstrap_finish (GPasteClipboard *self,
                                    GPasteHistory   *history)
{
    GPasteClipboardPrivate  *priv = g_paste_clipboard_get_instance_private (self);

    if (!priv->text && !priv->image_checksum)
    {
        const GSList *h = g_paste_history_get_history (history);
        if (h)
            g_paste_clipboard_select_item (self, h->data);
    }
}

static void
g_paste_clipboard_bootstrap_finish_text (GPasteClipboard *self,
                                         const gchar     *text G_GNUC_UNUSED,
                                         gpointer         user_data)
{
    g_paste_clipboard_bootstrap_finish (self, user_data);
}

static void
g_paste_clipboard_bootstrap_finish_image (GPasteClipboard *self,
                                          GdkPixbuf       *image G_GNUC_UNUSED,
                                          gpointer         user_data)
{
    g_paste_clipboard_bootstrap_finish (self, user_data);
}

/**
 * g_paste_clipboard_bootstrap:
 * @self: a #GPasteClipboard instance
 * @history: a #GPasteHistory instance
 *
 * Bootstrap a #GPasteClipboard with an initial value
 *
 * Returns:
 */
G_PASTE_VISIBLE void
g_paste_clipboard_bootstrap (GPasteClipboard *self,
                             GPasteHistory   *history)
{
    g_return_if_fail (G_PASTE_IS_CLIPBOARD (self));
    g_return_if_fail (G_PASTE_IS_HISTORY (history));

    GPasteClipboardPrivate  *priv = g_paste_clipboard_get_instance_private (self);
    GtkClipboard *real = priv->real;

    if (gtk_clipboard_wait_is_uris_available (real) ||
        gtk_clipboard_wait_is_text_available (real))
    {
        g_paste_clipboard_set_text (self,
                                    g_paste_clipboard_bootstrap_finish_text,
                                    history);
    }
    else if (gtk_clipboard_wait_is_image_available (real))
    {
        g_paste_clipboard_set_image (self,
                                     g_paste_clipboard_bootstrap_finish_image,
                                     history);
    }
}

/**
 * g_paste_clipboard_get_target:
 * @self: a #GPasteClipboard instance
 *
 * Get the target the #GPasteClipboard points to
 *
 * Returns: (transfer none): the GdkAtom representing the target (Primary, Clipboard, ...)
 */
G_PASTE_VISIBLE GdkAtom
g_paste_clipboard_get_target (const GPasteClipboard *self)
{
    g_return_val_if_fail (G_PASTE_IS_CLIPBOARD (self), NULL);

    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private ((GPasteClipboard *) self);

    return priv->target;
}

/**
 * g_paste_clipboard_get_real:
 * @self: a #GPasteClipboard instance
 *
 * Get the GtkClipboard linked to the #GPasteClipboard
 *
 * Returns: (transfer none): the GtkClipboard used in the #GPasteClipboard
 */
G_PASTE_VISIBLE GtkClipboard *
g_paste_clipboard_get_real (const GPasteClipboard *self)
{
    g_return_val_if_fail (G_PASTE_IS_CLIPBOARD (self), NULL);

    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private ((GPasteClipboard *) self);

    return priv->real;
}

/**
 * g_paste_clipboard_get_text:
 * @self: a #GPasteClipboard instance
 *
 * Get the text stored in the #GPasteClipboard
 *
 * Returns: read-only string containing the text or NULL
 */
G_PASTE_VISIBLE const gchar *
g_paste_clipboard_get_text (const GPasteClipboard *self)
{
    g_return_val_if_fail (G_PASTE_IS_CLIPBOARD (self), NULL);

    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private ((GPasteClipboard *) self);

    return priv->text;
}

static void
g_paste_clipboard_private_set_text (GPasteClipboardPrivate *priv,
                                    const gchar            *text)
{
    g_free (priv->text);
    g_free (priv->image_checksum);

    priv->text = g_strdup (text);
    priv->image_checksum = NULL;
}

typedef struct {
    GPasteClipboard            *self;
    GPasteClipboardTextCallback callback;
    gpointer                    user_data;
} GPasteClipboardTextCallbackData;

static void
g_paste_clipboard_on_text_ready (GtkClipboard *clipboard G_GNUC_UNUSED,
                                 const gchar  *text,
                                 gpointer      user_data)
{
    G_PASTE_CLEANUP_FREE GPasteClipboardTextCallbackData *data = user_data;
    GPasteClipboard *self = data->self;

    if (!text)
    {
        if (data->callback)
            data->callback (self, NULL, data->user_data);
        return;
    }

    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private (self);
    GPasteSettings *settings = priv->settings;
    G_PASTE_CLEANUP_FREE gchar *stripped = g_strstrip (g_strdup (text));
    gboolean trim_items = g_paste_settings_get_trim_items (settings);
    const gchar *to_add = trim_items ? stripped : text;
    gsize length = strlen (to_add);

    if (length < g_paste_settings_get_min_text_item_size (settings) ||
        length > g_paste_settings_get_max_text_item_size (settings) ||
        !strlen (stripped))
    {
        if (data->callback)
            data->callback (self, NULL, data->user_data);
        return;
    }
    if (priv->text && !g_strcmp0 (priv->text, to_add))
    {
        if (data->callback)
            data->callback (self, NULL, data->user_data);
        return;
    }

    if (trim_items &&
        priv->target == GDK_SELECTION_CLIPBOARD &&
        g_strcmp0 (text, stripped))
            g_paste_clipboard_select_text (self, stripped);
    else
        g_paste_clipboard_private_set_text (priv, to_add);

    if (data->callback)
        data->callback (self, priv->text, data->user_data);
}

/**
 * g_paste_clipboard_set_text:
 * @self: a #GPasteClipboard instance
 * @callback: (scope async): the callback to be called when text is received
 * @user_data: user data to pass to @callback
 *
 * Put the text from the intern GtkClipboard in the #GPasteClipboard
 *
 * Returns:
 */
G_PASTE_VISIBLE void
g_paste_clipboard_set_text (GPasteClipboard            *self,
                            GPasteClipboardTextCallback callback,
                            gpointer                    user_data)
{
    g_return_if_fail (G_PASTE_IS_CLIPBOARD (self));

    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private (self);
    GPasteClipboardTextCallbackData *data = g_new (GPasteClipboardTextCallbackData, 1);

    data->self = self;
    data->callback = callback;
    data->user_data = user_data;

    gtk_clipboard_request_text (priv->real,
                                g_paste_clipboard_on_text_ready,
                                data);
}

/**
 * g_paste_clipboard_select_text:
 * @self: a #GPasteClipboard instance
 * @text: the text to select
 *
 * Put the text into the #GPasteClipbaord and the intern GtkClipboard
 *
 * Returns:
 */
G_PASTE_VISIBLE void
g_paste_clipboard_select_text (GPasteClipboard *self,
                               const gchar     *text)
{
    g_return_if_fail (G_PASTE_IS_CLIPBOARD (self));
    g_return_if_fail (text);
    g_return_if_fail (g_utf8_validate (text, -1, NULL));

    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private (self);
    GtkClipboard *real = priv->real;

    /* Avoid cycling twice */
    g_paste_clipboard_private_set_text (priv, text);

    /* Let the clipboards manager handle our internal text */
    gtk_clipboard_set_text (real, text, -1);
    gtk_clipboard_store (real);
}

static void
g_paste_clipboard_get_clipboard_data (GtkClipboard     *clipboard G_GNUC_UNUSED,
                                      GtkSelectionData *selection_data,
                                      guint             info G_GNUC_UNUSED,
                                      gpointer          user_data_or_owner)
{
    g_return_if_fail (G_PASTE_IS_ITEM (user_data_or_owner));

    GPasteItem *item = G_PASTE_ITEM (user_data_or_owner);

    GdkAtom targets[1] = { gtk_selection_data_get_target (selection_data) };

    /* The content is requested as text */
    if (gtk_targets_include_text (targets, 1))
        gtk_selection_data_set_text (selection_data, g_paste_item_get_real_value (item), -1);
    else if (G_PASTE_IS_IMAGE_ITEM (item))
    {
        if (gtk_targets_include_image (targets, 1, TRUE))
            gtk_selection_data_set_pixbuf (selection_data, g_paste_image_item_get_image (G_PASTE_IMAGE_ITEM (item)));
    }
    /* The content is requested as uris */
    else
    {
        g_return_if_fail (G_PASTE_IS_URIS_ITEM (item));

        const gchar * const *uris = g_paste_uris_item_get_uris (G_PASTE_URIS_ITEM (item));

        if (gtk_targets_include_uri (targets, 1))
            gtk_selection_data_set_uris (selection_data, (GStrv) uris);
        /* The content is requested as special gnome-copied-files by nautilus */
        else
        {
            G_PASTE_CLEANUP_STRING_FREE GString *copy_string = g_string_new ("copy");
            guint length = g_strv_length ((GStrv) uris);

            for (guint i = 0; i < length; ++i)
            {
                g_string_append (g_string_append (copy_string,
                                                  "\n"),
                                 uris[i]);
            }

            gchar *str = copy_string->str;
            length = copy_string->len + 1;
            G_PASTE_CLEANUP_FREE guchar *copy_files_data = g_new (guchar, length);
            for (guint i = 0; i < length; ++i)
                copy_files_data[i] = (guchar) str[i];
            gtk_selection_data_set (selection_data, g_paste_clipboard_copy_files_target, 8, copy_files_data, length);
        }
    }
}

static void
g_paste_clipboard_clear_clipboard_data (GtkClipboard *clipboard G_GNUC_UNUSED,
                                        gpointer      user_data_or_owner)
{
    g_object_unref (user_data_or_owner);
}

static void
g_paste_clipboard_private_select_uris (GPasteClipboardPrivate *priv,
                                       GPasteUrisItem         *item)
{
    GtkClipboard *real = priv->real;
    G_PASTE_CLEANUP_TARGETS_UNREF GtkTargetList *target_list = gtk_target_list_new (NULL, 0);

    g_paste_clipboard_private_set_text (priv, g_paste_item_get_real_value (G_PASTE_ITEM (item)));

    gtk_target_list_add_text_targets (target_list, 0);
    gtk_target_list_add_uri_targets (target_list, 0);
    gtk_target_list_add (target_list, g_paste_clipboard_copy_files_target, 0, 0);

    gint n_targets;
    GtkTargetEntry *targets = gtk_target_table_new_from_list (target_list, &n_targets);
    gtk_clipboard_set_with_owner (real,
                                  targets,
                                  n_targets,
                                  g_paste_clipboard_get_clipboard_data,
                                  g_paste_clipboard_clear_clipboard_data,
                                  g_object_ref (item));
    gtk_clipboard_store (real);

    gtk_target_table_free (targets, n_targets);
}

/**
 * g_paste_clipboard_get_image_checksum:
 * @self: a #GPasteClipboard instance
 *
 * Get the checksum of the image stored in the #GPasteClipboard
 *
 * Returns: read-only string containing the checksum of the image stored in the #GPasteClipboard or NULL
 */
G_PASTE_VISIBLE const gchar *
g_paste_clipboard_get_image_checksum (const GPasteClipboard *self)
{
    g_return_val_if_fail (G_PASTE_IS_CLIPBOARD (self), NULL);

    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private ((GPasteClipboard *) self);

    return priv->image_checksum;
}

static void
g_paste_clipboard_private_set_image_checksum (GPasteClipboardPrivate *priv,
                                              const gchar            *image_checksum)
{
    g_free (priv->text);
    g_free (priv->image_checksum);

    priv->text = NULL;
    priv->image_checksum = g_strdup (image_checksum);
}

static void
g_paste_clipboard_private_select_image (GPasteClipboardPrivate *priv,
                                        GdkPixbuf              *image,
                                        const gchar            *checksum)
{
    g_return_if_fail (GDK_IS_PIXBUF (image));

    GtkClipboard *real = priv->real;

    g_paste_clipboard_private_set_image_checksum (priv, checksum);
    gtk_clipboard_set_image (real, image);
}

typedef struct {
    GPasteClipboard             *self;
    GPasteClipboardImageCallback callback;
    gpointer                     user_data;
} GPasteClipboardImageCallbackData;

static void
g_paste_clipboard_on_image_ready (GtkClipboard *clipboard G_GNUC_UNUSED,
                                  GdkPixbuf    *image,
                                  gpointer      user_data)
{
    G_PASTE_CLEANUP_FREE GPasteClipboardImageCallbackData *data = user_data;
    GPasteClipboard *self = data->self;

    if (!image)
    {
        if (data->callback)
            data->callback (self, NULL, data->user_data);
        return;
    }

    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private (self);

    G_PASTE_CLEANUP_FREE gchar *checksum = g_compute_checksum_for_data (G_CHECKSUM_SHA256,
                                                                        (guchar *) gdk_pixbuf_get_pixels (image),
                                                                        -1);

    if (g_strcmp0 (checksum, priv->image_checksum))
    {
        g_paste_clipboard_private_select_image (priv,
                                                image,
                                                checksum);
    }
    else
    {
        image = NULL;
    }

    if (data->callback)
        data->callback (self, image, data->user_data);
}

/**
 * g_paste_clipboard_set_image:
 * @self: a #GPasteClipboard instance
 * @callback: (scope async): the callback to be called when text is received
 * @user_data: user data to pass to @callback
 *
 * Put the image from the intern GtkClipboard in the #GPasteClipboard
 *
 * Returns:
 */
G_PASTE_VISIBLE void
g_paste_clipboard_set_image (GPasteClipboard             *self,
                             GPasteClipboardImageCallback callback,
                             gpointer                     user_data)
{
    g_return_if_fail (G_PASTE_IS_CLIPBOARD (self));

    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private (self);
    GPasteClipboardImageCallbackData *data = g_new (GPasteClipboardImageCallbackData, 1);

    data->self = self;
    data->callback = callback;
    data->user_data = user_data;

    gtk_clipboard_request_image (priv->real,
                                 g_paste_clipboard_on_image_ready,
                                 data);
}

/**
 * g_paste_clipboard_select_item:
 * @self: a #GPasteClipboard instance
 * @item: the item to select
 *
 * Put the value of the item into the #GPasteClipbaord and the intern GtkClipboard
 *
 * Returns:
 */
G_PASTE_VISIBLE void
g_paste_clipboard_select_item (GPasteClipboard  *self,
                               const GPasteItem *item)
{
    g_return_if_fail (G_PASTE_IS_CLIPBOARD (self));
    g_return_if_fail (G_PASTE_IS_ITEM (item));

    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private (self);

    if (G_PASTE_IS_IMAGE_ITEM (item))
    {
        GPasteImageItem *image_item = G_PASTE_IMAGE_ITEM (item);
        const gchar *checksum = g_paste_image_item_get_checksum (image_item);

        if (g_strcmp0 (checksum, priv->image_checksum))
        {
            g_paste_clipboard_private_select_image (priv,
                                                    g_paste_image_item_get_image (image_item),
                                                    checksum);
        }
    }
    else
    {
        const gchar *text = g_paste_item_get_real_value (item);

        if (g_strcmp0 (text, priv->text))
        {
            if (G_PASTE_IS_URIS_ITEM (item))
                g_paste_clipboard_private_select_uris (priv, G_PASTE_URIS_ITEM (item));
            else  if (G_PASTE_IS_TEXT_ITEM (item))
                g_paste_clipboard_select_text (self, text);
            else
                g_assert_not_reached ();
        }
    }
}

static gchar *
_get_actor_name_from_pid (pid_t pid)
{
    char         *link_file     = NULL;
    char         *link_target   = NULL;
    char        **split_target  = NULL;
    char         *actor_name    = NULL;
    ssize_t       read_len      = PATH_MAX; // 4096, so it's unlikely link_len ever overflows :)
    ssize_t       link_len      = 1;

    if (pid <= 0)
    {
        return NULL;
    }

    link_file = g_strdup_printf ("/proc/%d/exe", pid);
    if (link_file == NULL)
    {
        return NULL;
    }

    // It is impossible to obtain the size of /proc link targets as /proc is
    // not POSIX compliant. Hence, we start from the NAME_MAX limit and increase
    // it all the way up to readlink failing. readlink will fail upon reaching
    // the PATH_MAX limit on Linux implementations. read_len will be strictly
    // inferior to link_len as soon as the latter is big enough to contain the
    // path to the executable and a trailing null character.
    while (read_len >= link_len)
    {
        link_len += NAME_MAX;

        g_free(link_target);
        link_target = g_malloc(link_len * sizeof (char));

        if (link_target == NULL)
        {
            g_free (link_file);
            g_free (link_target);
            return NULL;
        }

        read_len= readlink (link_file, link_target, link_len);

        if (read_len < 0)
        {
            g_free (link_file);
            g_free (link_target);
            return NULL;
        }
    }

    g_free (link_file);

    // readlink does not null-terminate the string
    link_target[read_len] = '\0';

    split_target = g_strsplit (link_target, "/", -1);
    g_free (link_target);

    if(split_target == NULL)
    {
        return NULL;
    }

    // Iterate to the last element which is the executable name
    for (read_len = 0; split_target[read_len]; read_len++);

    // Turn it into an arbitrary actor name
    actor_name = g_strdup_printf ("application://%s.desktop", split_target[--read_len]);
    g_strfreev (split_target);

    return actor_name;
}

/* Inspired by xdotool.
Copyright (c) 2007, 2008, 2009: Jordan Sissel.
Copyright (c) 2015: Steve Dodier-Lazaro <sidnioulz@gmail.com>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Jordan Sissel nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY JORDAN SISSEL ``AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL JORDAN SISSEL BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
static pid_t
_get_pid_from_xid (Display *xdsp,
                   Window   window)
{
    static Atom pid_atom = -1;
    if (pid_atom == (Atom)-1)
        pid_atom = XInternAtom(xdsp, "_NET_WM_PID", False);

    Atom actual_type;
    int actual_format;
    unsigned long _nitems;
    unsigned long _bytes_after;
    unsigned char *prop;
    int status;

    status = XGetWindowProperty(xdsp, window, pid_atom, 0, (~0L),
        False, AnyPropertyType, &actual_type, &actual_format,
        &_nitems, &_bytes_after, &prop);

    if (status != Success || !prop)
        return 0;
    else
        return prop[1]*256 + prop[0];
}

static void
_log_zeitgeist_copy_event (GdkEventOwnerChange *gdkevent,
                           GPasteClipboard     *self G_GNUC_UNUSED)
{
    // Get access to Zeitgeist logger daemon
    ZeitgeistLog *log = zeitgeist_log_get_default ();

    // Calculate information about the client
    GdkDisplay *dsp = gdk_display_get_default ();
    gchar *actor_name, *window_id;
    pid_t pid = 0;

    #ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY (dsp) && gdkevent->owner)
    {
        Display *xdsp = gdk_x11_display_get_xdisplay (dsp);
        Window xid = (Window) gdk_x11_window_get_xid (gdkevent->owner);

        pid = _get_pid_from_xid (xdsp, xid);
        window_id = g_strdup_printf ("%lu", xid);
        actor_name = _get_actor_name_from_pid (pid);
    }
    else
    #endif
    {
        window_id = g_strdup ("n/a");
        actor_name = g_strdup ("application://unknown.desktop");
    }

    // Create the event to be added, with the known information
    ZeitgeistEvent *event = zeitgeist_event_new_full (
        ZEITGEIST_ZG_CLIPBOARD_COPY_EVENT,
        ZEITGEIST_ZG_USER_ACTIVITY,
        actor_name,
        NULL);
    g_free (actor_name);

    // Add the UCL metadata
    char *study_uri = g_strdup_printf ("activity://null///pid://%d///winid://%s///", pid, window_id);
    ZeitgeistSubject *subject = zeitgeist_subject_new_full (study_uri,
        "activity://gui-toolkit/gpaste/Clipboard/Actor",
        ZEITGEIST_ZG_WORLD_ACTIVITY,
        "application/octet-stream",
        NULL,
        "ucl-study-metadata",
        NULL);
    zeitgeist_event_add_subject (event, subject);
    g_free (window_id);
    g_free (study_uri);

    // Send the event
    zeitgeist_log_insert_events_no_reply (log, event, NULL);
        
}

static void
g_paste_clipboard_owner_change (GtkClipboard        *clipboard G_GNUC_UNUSED,
                                GdkEventOwnerChange *event,
                                gpointer             user_data)
{
    GPasteClipboard *self = user_data;
    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private (self);

    // Study data logging
    if (priv->target == gdk_atom_intern ("CLIPBOARD", TRUE) && event->reason == GDK_OWNER_CHANGE_NEW_OWNER)
    {
        _log_zeitgeist_copy_event (event, self);
    }

    g_signal_emit (self,
		   signals[OWNER_CHANGE],
                   0, /* detail */
                   event,
                   NULL);
}

static void
g_paste_clipboard_fake_event_finish_text (GtkClipboard *clipboard G_GNUC_UNUSED,
                                          const gchar  *text,
                                          gpointer      user_data)
{
    GPasteClipboard *self = user_data;
    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private (self);

    if (g_strcmp0 (text, priv->text))
        g_paste_clipboard_owner_change (NULL, NULL, self);
}

/* FIXME: dedupe from gpaste-image-item */
static gchar *
image_compute_checksum (GdkPixbuf *image)
{
    if (!image)
        return NULL;

    guint length;
    const guchar *data = gdk_pixbuf_get_pixels_with_length (image,
                                                            &length);
    return g_compute_checksum_for_data (G_CHECKSUM_SHA256,
                                        data,
                                        length);
}

static void
g_paste_clipboard_fake_event_finish_image (GtkClipboard *clipboard G_GNUC_UNUSED,
                                           GdkPixbuf    *image,
                                           gpointer      user_data)
{
    GPasteClipboard *self = user_data;
    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private (self);
    G_PASTE_CLEANUP_FREE gchar *checksum = image_compute_checksum (image);

    if (g_strcmp0 (checksum, priv->image_checksum))
        g_paste_clipboard_owner_change (NULL, NULL, self);

    g_object_unref (image);
}

static gboolean
g_paste_clipboard_fake_event (gpointer user_data)
{
    GPasteClipboard *self = user_data;
    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private (self);

    if (priv->text)
        gtk_clipboard_request_text (priv->real, g_paste_clipboard_fake_event_finish_text, self);
    else if (priv->image_checksum)
        gtk_clipboard_request_image (priv->real, g_paste_clipboard_fake_event_finish_image, self);
    else
        g_paste_clipboard_owner_change (NULL, NULL, self);

    return G_SOURCE_CONTINUE;
}

static void
g_paste_clipboard_dispose (GObject *object)
{
    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private (G_PASTE_CLIPBOARD (object));

    if (priv->settings)
    {
        g_signal_handler_disconnect (priv->real, priv->owner_change_signal);
        g_clear_object (&priv->settings);
    }

    G_OBJECT_CLASS (g_paste_clipboard_parent_class)->dispose (object);
}

static void
g_paste_clipboard_finalize (GObject *object)
{
    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private (G_PASTE_CLIPBOARD (object));

    g_free (priv->text);
    g_free (priv->image_checksum);

    G_OBJECT_CLASS (g_paste_clipboard_parent_class)->finalize (object);
}

static void
g_paste_clipboard_class_init (GPasteClipboardClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = g_paste_clipboard_dispose;
    object_class->finalize = g_paste_clipboard_finalize;

    signals[OWNER_CHANGE] = g_signal_new ("owner-change",
                                          G_PASTE_TYPE_CLIPBOARD,
                                          G_SIGNAL_RUN_FIRST,
                                          0,    /* class offset     */
                                          NULL, /* accumulator      */
                                          NULL, /* accumulator data */
                                          g_cclosure_marshal_VOID__BOXED,
                                          G_TYPE_NONE,
                                          1,
                                          GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);
}

static void
g_paste_clipboard_init (GPasteClipboard *self)
{
    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private (self);

    priv->text = NULL;
    priv->image_checksum = NULL;
}

/**
 * g_paste_clipboard_new:
 * @target: the GdkAtom representating the GtkClipboard we're abstracting
 * @settings: a #GPasteSettings instance
 *
 * Create a new instance of #GPasteClipboard
 *
 * Returns: a newly allocated #GPasteClipboard
 *          free it with g_object_unref
 */
G_PASTE_VISIBLE GPasteClipboard *
g_paste_clipboard_new (GdkAtom         target,
                       GPasteSettings *settings)
{
    g_return_val_if_fail (G_PASTE_IS_SETTINGS (settings), NULL);

    GPasteClipboard *self = g_object_new (G_PASTE_TYPE_CLIPBOARD, NULL);
    GPasteClipboardPrivate *priv = g_paste_clipboard_get_instance_private (self);

    priv->target = target;
    priv->settings = g_object_ref (settings);

    GtkClipboard *real = priv->real = gtk_clipboard_get (target);

    priv->owner_change_signal = g_signal_connect (real,
                                                  "owner-change",
                                                  G_CALLBACK (g_paste_clipboard_owner_change),
                                                  self);

    if (!gdk_display_request_selection_notification (gdk_display_get_default (), target))
    {
        g_warning ("Selection notification not supported, using active poll");
        g_source_set_name_by_id (g_timeout_add_seconds (1, g_paste_clipboard_fake_event, self), "[GPaste] clipboard fake events");
    }

    return self;
}
