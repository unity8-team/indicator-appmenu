/*
An implementation of indicator object showing menus from applications.

Copyright 2010 Canonical Ltd.

Authors:
    Ted Gould <ted@canonical.com>

This program is free software: you can redistribute it and/or modify it 
under the terms of the GNU General Public License version 3, as published 
by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but 
WITHOUT ANY WARRANTY; without even the implied warranties of 
MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR 
PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along 
with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <X11/Xlib.h>
#include <gdk/gdkx.h>
#include <gio/gio.h>

#include <libindicator/indicator.h>
#include <libindicator/indicator-object.h>

#include <libdbusmenu-glib/menuitem.h>
#include <libdbusmenu-glib/client.h>

#include <libbamf/bamf-matcher.h>

#include "gen-application-menu-registrar.xml.h"
#include "gen-application-menu-renderer.xml.h"
#include "indicator-appmenu-marshal.h"
#include "window-menus.h"
#include "dbus-shared.h"
#include "gdk-get-func.h"

/**********************
  Indicator Object
 **********************/
#define INDICATOR_APPMENU_TYPE            (indicator_appmenu_get_type ())
#define INDICATOR_APPMENU(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), INDICATOR_APPMENU_TYPE, IndicatorAppmenu))
#define INDICATOR_APPMENU_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), INDICATOR_APPMENU_TYPE, IndicatorAppmenuClass))
#define IS_INDICATOR_APPMENU(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), INDICATOR_APPMENU_TYPE))
#define IS_INDICATOR_APPMENU_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), INDICATOR_APPMENU_TYPE))
#define INDICATOR_APPMENU_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), INDICATOR_APPMENU_TYPE, IndicatorAppmenuClass))

GType indicator_appmenu_get_type (void);

INDICATOR_SET_VERSION
INDICATOR_SET_TYPE(INDICATOR_APPMENU_TYPE)

typedef struct _IndicatorAppmenu      IndicatorAppmenu;
typedef struct _IndicatorAppmenuClass IndicatorAppmenuClass;
typedef struct _IndicatorAppmenuDebug      IndicatorAppmenuDebug;
typedef struct _IndicatorAppmenuDebugClass IndicatorAppmenuDebugClass;

typedef enum _ActiveStubsState ActiveStubsState;
enum _ActiveStubsState {
	STUBS_UNKNOWN,
	STUBS_SHOW,
	STUBS_HIDE
};

struct _IndicatorAppmenuClass {
	IndicatorObjectClass parent_class;

	void (*window_registered) (IndicatorAppmenu * iapp, guint wid, gchar * address, gpointer path, gpointer user_data);
	void (*window_unregistered) (IndicatorAppmenu * iapp, guint wid, gpointer user_data);
};

struct _IndicatorAppmenu {
	IndicatorObject parent;

	gulong retry_registration;

	WindowMenus * default_app;
	GHashTable * apps;

	BamfMatcher * matcher;
	BamfWindow * active_window;
	ActiveStubsState active_stubs;

	gulong sig_entry_added;
	gulong sig_entry_removed;
	gulong sig_show_menu;

	GtkMenuItem * close_item;

	GArray * window_menus;

	GHashTable * desktop_windows;
	WindowMenus * desktop_menu;

	IndicatorAppmenuDebug * debug;

	GDBusConnection * bus;
	guint owner_id;
	guint dbus_registration;
};


/**********************
  Debug Proxy
 **********************/
#define INDICATOR_APPMENU_DEBUG_TYPE            (indicator_appmenu_debug_get_type ())
#define INDICATOR_APPMENU_DEBUG(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), INDICATOR_APPMENU_DEBUG_TYPE, IndicatorAppmenuDebug))
#define INDICATOR_APPMENU_DEBUG_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), INDICATOR_APPMENU_DEBUG_TYPE, IndicatorAppmenuDebugClass))
#define IS_INDICATOR_APPMENU_DEBUG(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), INDICATOR_APPMENU_DEBUG_TYPE))
#define IS_INDICATOR_APPMENU_DEBUG_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), INDICATOR_APPMENU_DEBUG_TYPE))
#define INDICATOR_APPMENU_DEBUG_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), INDICATOR_APPMENU_DEBUG_TYPE, IndicatorAppmenuDebugClass))

GType indicator_appmenu_debug_get_type (void);

struct _IndicatorAppmenuDebugClass {
	GObjectClass parent_class;
};

struct _IndicatorAppmenuDebug {
	GObject parent;
	IndicatorAppmenu * appmenu;
	GCancellable * bus_cancel;
	GDBusConnection * bus;
	guint dbus_registration;
};


/**********************
  Prototypes
 **********************/
static void indicator_appmenu_class_init                             (IndicatorAppmenuClass *klass);
static void indicator_appmenu_init                                   (IndicatorAppmenu *self);
static void indicator_appmenu_dispose                                (GObject *object);
static void indicator_appmenu_finalize                               (GObject *object);
static void indicator_appmenu_debug_class_init                       (IndicatorAppmenuDebugClass *klass);
static void indicator_appmenu_debug_init                             (IndicatorAppmenuDebug *self);
static void indicator_appmenu_debug_dispose                          (GObject *object);
static void build_window_menus                                       (IndicatorAppmenu * iapp);
static GList * get_entries                                           (IndicatorObject * io);
static guint get_location                                            (IndicatorObject * io,
                                                                      IndicatorObjectEntry * entry);
static void entry_activate                                           (IndicatorObject * io,
                                                                      IndicatorObjectEntry * entry,
                                                                      guint timestamp);
static void switch_default_app                                       (IndicatorAppmenu * iapp,
                                                                      WindowMenus * newdef,
                                                                      BamfWindow * active_window);
static void find_desktop_windows                                     (IndicatorAppmenu * iapp);
static void new_window                                               (BamfMatcher * matcher,
                                                                      BamfView * view,
                                                                      gpointer user_data);
static void old_window                                               (BamfMatcher * matcher,
                                                                      BamfView * view,
                                                                      gpointer user_data);
static void window_entry_added                                       (WindowMenus * mw,
                                                                      IndicatorObjectEntry * entry,
                                                                      gpointer user_data);
static void window_entry_removed                                     (WindowMenus * mw,
                                                                      IndicatorObjectEntry * entry,
                                                                      gpointer user_data);
static void window_show_menu                                         (WindowMenus * mw,
                                                                      IndicatorObjectEntry * entry,
                                                                      guint timestamp,
                                                                      gpointer user_data);
static void active_window_changed                                    (BamfMatcher * matcher,
                                                                      BamfView * oldview,
                                                                      BamfView * newview,
                                                                      gpointer user_data);
static GQuark error_quark                                            (void);
static gboolean retry_registration                                   (gpointer user_data);
static void bus_method_call                                          (GDBusConnection * connection,
                                                                      const gchar * sender,
                                                                      const gchar * path,
                                                                      const gchar * interface,
                                                                      const gchar * method,
                                                                      GVariant * params,
                                                                      GDBusMethodInvocation * invocation,
                                                                      gpointer user_data);
static void on_bus_acquired                                          (GDBusConnection * connection,
                                                                      const gchar * name,
                                                                      gpointer user_data);
static void on_name_acquired                                         (GDBusConnection * connection,
                                                                      const gchar * name,
                                                                      gpointer user_data);
static void on_name_lost                                             (GDBusConnection * connection,
                                                                      const gchar * name,
                                                                      gpointer user_data);
static void dbg_bus_method_call                                      (GDBusConnection * connection,
                                                                      const gchar * sender,
                                                                      const gchar * path,
                                                                      const gchar * interface,
                                                                      const gchar * method,
                                                                      GVariant * params,
                                                                      GDBusMethodInvocation * invocation,
                                                                      gpointer user_data);
static void dbg_bus_get_cb                                           (GObject * object,
                                                                      GAsyncResult * res,
                                                                      gpointer user_data);

/* Unique error codes for debug interface */
enum {
	ERROR_NO_APPLICATIONS,
	ERROR_NO_DEFAULT_APP,
	ERROR_WINDOW_NOT_FOUND
};

/**********************
  DBus Interfaces
 **********************/
static GDBusNodeInfo *      node_info = NULL;
static GDBusInterfaceInfo * interface_info = NULL;
static GDBusInterfaceVTable interface_table = {
       method_call:    bus_method_call,
       get_property:   NULL, /* No properties */
       set_property:   NULL  /* No properties */
};

static GDBusNodeInfo *      dbg_node_info = NULL;
static GDBusInterfaceInfo * dbg_interface_info = NULL;
static GDBusInterfaceVTable dbg_interface_table = {
       method_call:    dbg_bus_method_call,
       get_property:   NULL, /* No properties */
       set_property:   NULL  /* No properties */
};

G_DEFINE_TYPE (IndicatorAppmenu, indicator_appmenu, INDICATOR_OBJECT_TYPE);

/* One time init */
static void
indicator_appmenu_class_init (IndicatorAppmenuClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->dispose = indicator_appmenu_dispose;
	object_class->finalize = indicator_appmenu_finalize;

	IndicatorObjectClass * ioclass = INDICATOR_OBJECT_CLASS(klass);

	ioclass->get_entries = get_entries;
	ioclass->get_location = get_location;
	ioclass->entry_activate = entry_activate;

	/* Setting up the DBus interfaces */
	if (node_info == NULL) {
		GError * error = NULL;

		node_info = g_dbus_node_info_new_for_xml(_application_menu_registrar, &error);
		if (error != NULL) {
			g_error("Unable to parse Application Menu Interface description: %s", error->message);
			g_error_free(error);
		}
	}

	if (interface_info == NULL) {
		interface_info = g_dbus_node_info_lookup_interface(node_info, REG_IFACE);

		if (interface_info == NULL) {
			g_error("Unable to find interface '" REG_IFACE "'");
		}
	}

	return;
}

/* Per instance Init */
static void
indicator_appmenu_init (IndicatorAppmenu *self)
{
	self->default_app = NULL;
	self->debug = NULL;
	self->apps = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_object_unref);
	self->matcher = NULL;
	self->active_window = NULL;
	self->active_stubs = STUBS_UNKNOWN;
	self->close_item = NULL;
	self->retry_registration = 0;
	self->bus = NULL;
	self->owner_id = 0;
	self->dbus_registration = 0;

	/* Setup the entries for the fallbacks */
	self->window_menus = g_array_sized_new(FALSE, FALSE, sizeof(IndicatorObjectEntry), 2);

	/* Setup the cache of windows with possible desktop entries */
	self->desktop_windows = g_hash_table_new(g_direct_hash, g_direct_equal);
	self->desktop_menu = NULL; /* Starts NULL until found */

	build_window_menus(self);

	/* Get the default BAMF matcher */
	self->matcher = bamf_matcher_get_default();
	if (self->matcher == NULL) {
		/* we don't want to exit out of Unity -- but this
		   should really never happen */
		g_warning("Unable to get BAMF matcher, can not watch applications switch!");
	} else {
		g_signal_connect(G_OBJECT(self->matcher), "active-window-changed", G_CALLBACK(active_window_changed), self);

		/* Desktop window tracking */
		g_signal_connect(G_OBJECT(self->matcher), "view-opened", G_CALLBACK(new_window), self);
		g_signal_connect(G_OBJECT(self->matcher), "view-closed", G_CALLBACK(old_window), self);
	}

	find_desktop_windows(self);

	/* Request a name so others can find us */
	retry_registration(self);

	/* Setup debug interface */
	self->debug = g_object_new(INDICATOR_APPMENU_DEBUG_TYPE, NULL);
	self->debug->appmenu = self;

	return;
}

/* If we weren't able to register on the bus, then we need
   to try it all again. */
static gboolean
retry_registration (gpointer user_data)
{
	g_return_val_if_fail(IS_INDICATOR_APPMENU(user_data), FALSE);
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(user_data);

	iapp->owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
	                                 DBUS_NAME,
	                                 G_BUS_NAME_OWNER_FLAGS_NONE,
	                                 iapp->dbus_registration == 0 ? on_bus_acquired : NULL,
	                                 on_name_acquired,
	                                 on_name_lost,
	                                 g_object_ref(iapp),
	                                 g_object_unref);

	return TRUE;
}

static void
on_bus_acquired (GDBusConnection * connection, const gchar * name,
                 gpointer user_data)
{
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(user_data);
	GError * error = NULL;

	iapp->bus = connection;

	/* Now register our object on our new connection */
	iapp->dbus_registration = g_dbus_connection_register_object(connection,
	                                                            REG_OBJECT,
	                                                            interface_info,
	                                                            &interface_table,
	                                                            user_data,
	                                                            NULL,
	                                                            &error);

	if (error != NULL) {
		g_error("Unable to register the object to DBus: %s", error->message);
		g_error_free(error);
		g_bus_unown_name(iapp->owner_id);
		iapp->owner_id = 0;
		iapp->retry_registration = g_timeout_add_seconds(1, retry_registration, iapp);
		return;
	}

	return;	
}

static void
on_name_acquired (GDBusConnection * connection, const gchar * name,
                  gpointer user_data)
{
}

static void
on_name_lost (GDBusConnection * connection, const gchar * name,
              gpointer user_data)
{
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(user_data);

	if (connection == NULL) {
		g_error("OMG! Unable to get a connection to DBus");
	}
	else {
		g_error("Unable to claim the name %s", DBUS_NAME);
	}

	/* We can rest assured no one will register with us, but let's
	   just ensure we're not showing anything. */
	switch_default_app(iapp, NULL, NULL);

	iapp->owner_id = 0;
}

/* Object refs decrement */
static void
indicator_appmenu_dispose (GObject *object)
{
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(object);

	/* Don't register if we're dying! */
	if (iapp->retry_registration != 0) {
		g_source_remove(iapp->retry_registration);
		iapp->retry_registration = 0;
	}

	if (iapp->dbus_registration != 0) {
		g_dbus_connection_unregister_object(iapp->bus, iapp->dbus_registration);
		/* Don't care if it fails, there's nothing we can do */
		iapp->dbus_registration = 0;
	}

	if (iapp->bus != NULL) {
		g_object_unref(iapp->bus);
		iapp->bus = NULL;
	}

	if (iapp->owner_id != 0) {
		g_bus_unown_name(iapp->owner_id);
		iapp->owner_id = 0;
	}

	/* bring down the matcher before resetting to no menu so we don't
	   get match signals */
	if (iapp->matcher != NULL) {
		g_object_unref(iapp->matcher);
		iapp->matcher = NULL;
	}

	/* No specific ref */
	switch_default_app (iapp, NULL, NULL);

	if (iapp->apps != NULL) {
		g_hash_table_destroy(iapp->apps);
		iapp->apps = NULL;
	}

	if (iapp->debug != NULL) {
		g_object_unref(iapp->debug);
		iapp->debug = NULL;
	}

	if (iapp->desktop_windows != NULL) {
		g_hash_table_destroy(iapp->desktop_windows);
		iapp->desktop_windows = NULL;
	}

	if (iapp->desktop_menu != NULL) {
		/* Wait, nothing here?  Yup.  We're not referencing the
		   menus here they're already attached to the window ID.
		   We're just keeping an efficient pointer to them. */
		iapp->desktop_menu = NULL;
	}

	G_OBJECT_CLASS (indicator_appmenu_parent_class)->dispose (object);
	return;
}

/* Free memory */
static void
indicator_appmenu_finalize (GObject *object)
{
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(object);

	if (iapp->window_menus != NULL) {
		if (iapp->window_menus->len != 0) {
			g_warning("Window menus weren't free'd in dispose!");
		}
		g_array_free(iapp->window_menus, TRUE);
		iapp->window_menus = NULL;
	}

	G_OBJECT_CLASS (indicator_appmenu_parent_class)->finalize (object);
	return;
}

G_DEFINE_TYPE (IndicatorAppmenuDebug, indicator_appmenu_debug, G_TYPE_OBJECT);

/* One time init */
static void
indicator_appmenu_debug_class_init (IndicatorAppmenuDebugClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->dispose = indicator_appmenu_debug_dispose;

	/* Setting up the DBus interfaces */
	if (dbg_node_info == NULL) {
		GError * error = NULL;

		dbg_node_info = g_dbus_node_info_new_for_xml(_application_menu_renderer, &error);
		if (error != NULL) {
			g_error("Unable to parse Application Menu Renderer Interface description: %s", error->message);
			g_error_free(error);
		}
	}

	if (dbg_interface_info == NULL) {
		dbg_interface_info = g_dbus_node_info_lookup_interface(dbg_node_info, DEBUG_IFACE);

		if (dbg_interface_info == NULL) {
			g_error("Unable to find interface '" DEBUG_IFACE "'");
		}
	}

	return;
}

/* Per instance Init */
static void
indicator_appmenu_debug_init (IndicatorAppmenuDebug *self)
{
	self->appmenu = NULL;
	self->bus_cancel = NULL;
	self->bus = NULL;
	self->dbus_registration = 0;

	/* Register this object on DBus */
	self->bus_cancel = g_cancellable_new();
	g_bus_get(G_BUS_TYPE_SESSION,
	          self->bus_cancel,
	          dbg_bus_get_cb,
	          self);

	return;
}

static void
dbg_bus_get_cb (GObject * object, GAsyncResult * res, gpointer user_data)
{
	GError * error = NULL;
	GDBusConnection * connection = g_bus_get_finish(res, &error);

	if (error != NULL) {
		g_error("OMG! Unable to get a connection to DBus: %s", error->message);
		g_error_free(error);
		return;
	}

	IndicatorAppmenuDebug * iappd = (IndicatorAppmenuDebug *)user_data;

	g_warn_if_fail(iappd->bus == NULL);
	iappd->bus = connection;

	if (iappd->bus_cancel != NULL) {
		g_object_unref(iappd->bus_cancel);
		iappd->bus_cancel = NULL;
	}

	/* Now register our object on our new connection */
	iappd->dbus_registration = g_dbus_connection_register_object(iappd->bus,
	                                                             DEBUG_OBJECT,
	                                                             dbg_interface_info,
	                                                             &dbg_interface_table,
	                                                             user_data,
	                                                             NULL,
	                                                             &error);

	if (error != NULL) {
		g_error("Unable to register the object to DBus: %s", error->message);
		g_error_free(error);
		return;
	}

	return;	
}

/* Object refs decrement */
static void
indicator_appmenu_debug_dispose (GObject *object)
{
	IndicatorAppmenuDebug * iappd = INDICATOR_APPMENU_DEBUG(object);

	if (iappd->dbus_registration != 0) {
		g_dbus_connection_unregister_object(iappd->bus, iappd->dbus_registration);
		/* Don't care if it fails, there's nothing we can do */
		iappd->dbus_registration = 0;
	}

	if (iappd->bus != NULL) {
		g_object_unref(iappd->bus);
		iappd->bus = NULL;
	}

	if (iappd->bus_cancel != NULL) {
		g_cancellable_cancel(iappd->bus_cancel);
		g_object_unref(iappd->bus_cancel);
		iappd->bus_cancel = NULL;
	}

	G_OBJECT_CLASS (indicator_appmenu_debug_parent_class)->dispose (object);
	return;
}

static void
emit_signal (IndicatorAppmenu * iapp, const gchar * name, GVariant * variant)
{
	GError * error = NULL;

	g_dbus_connection_emit_signal (iapp->bus,
		                       NULL,
		                       REG_OBJECT,
		                       REG_IFACE,
		                       name,
		                       variant,
		                       &error);

	if (error != NULL) {
		g_error("Unable to send %s signal: %s", name, error->message);
		g_error_free(error);
		return;
	}

	return;
}

/* Close the current application using magic */
static void
close_current (GtkMenuItem * mi, gpointer user_data)
{
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(user_data);

	if (!BAMF_IS_WINDOW (iapp->active_window) || bamf_view_is_closed (BAMF_VIEW (iapp->active_window))) {
		g_warning("Can't close a window we don't have. Window is either non-existent or recently closed.");
		return;
	}

	guint xid = bamf_window_get_xid(iapp->active_window);
	guint timestamp = gdk_event_get_time(NULL);

	XEvent xev;

	xev.xclient.type = ClientMessage;
	xev.xclient.serial = 0;
	xev.xclient.send_event = True;
	xev.xclient.display = gdk_x11_get_default_xdisplay ();
	xev.xclient.window = xid;
	xev.xclient.message_type = gdk_x11_atom_to_xatom (gdk_atom_intern ("_NET_CLOSE_WINDOW", TRUE));
	xev.xclient.format = 32;
	xev.xclient.data.l[0] = timestamp;
	xev.xclient.data.l[1] = 2; /* Client type pager, so it listens to us */
	xev.xclient.data.l[2] = 0;
	xev.xclient.data.l[3] = 0;
	xev.xclient.data.l[4] = 0;

	gdk_error_trap_push ();
	XSendEvent (gdk_x11_get_default_xdisplay (),
	            gdk_x11_get_default_root_xwindow (),
	            False,
	            SubstructureRedirectMask | SubstructureNotifyMask,
	            &xev);
  gdk_flush ();
	gdk_error_trap_pop ();

	return;
}

/* Create the default window menus */
static void
build_window_menus (IndicatorAppmenu * iapp)
{
	IndicatorObjectEntry entries[1] = {{0}};
	GtkAccelGroup * agroup = gtk_accel_group_new();
	GtkMenuItem * mi = NULL;
	GtkStockItem stockitem;

	/* File Menu */
	if (!gtk_stock_lookup(GTK_STOCK_FILE, &stockitem)) {
		g_warning("Unable to find the file menu stock item");
		stockitem.label = "_File";
	}
	entries[0].label = GTK_LABEL(gtk_label_new_with_mnemonic(stockitem.label));
	g_object_ref(G_OBJECT(entries[0].label));
	gtk_widget_show(GTK_WIDGET(entries[0].label));

	entries[0].menu = GTK_MENU(gtk_menu_new());
	g_object_ref(G_OBJECT(entries[0].menu));

	mi = GTK_MENU_ITEM(gtk_image_menu_item_new_from_stock(GTK_STOCK_CLOSE, agroup));
	gtk_widget_set_sensitive(GTK_WIDGET(mi), FALSE);
	g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(close_current), iapp);
	gtk_widget_show(GTK_WIDGET(mi));
	gtk_menu_shell_append(GTK_MENU_SHELL(entries[0].menu), GTK_WIDGET(mi));
	iapp->close_item = mi;

	gtk_widget_show(GTK_WIDGET(entries[0].menu));

	/* Copy the entries on the stack into the array */
	g_array_insert_vals(iapp->window_menus, 0, entries, 1);

	return;
}

/* Determine which windows should be used as the desktop
   menus. */
static void
determine_new_desktop (IndicatorAppmenu * iapp)
{
	GList * keys = g_hash_table_get_keys(iapp->desktop_windows);
	GList * key;

	for (key = keys; key != NULL; key = g_list_next(key)) {
		guint xid = GPOINTER_TO_UINT(key->data);
		gpointer pwm = g_hash_table_lookup(iapp->apps, GUINT_TO_POINTER(xid));
		if (pwm != NULL) {
			g_debug("Setting Desktop Menus to: %X", xid);
			iapp->desktop_menu = WINDOW_MENUS(pwm);
		}
	}

	g_list_free(keys);

	return;
}

/* Puts all the desktop windows into the hash table so that we
   can have a nice list of them. */
static void
find_desktop_windows (IndicatorAppmenu * iapp)
{
	GList * windows = bamf_matcher_get_windows(iapp->matcher);
	GList * lwindow;

	for (lwindow = windows; lwindow != NULL; lwindow = g_list_next(lwindow)) {
		BamfView * view = BAMF_VIEW(lwindow->data);
		new_window(iapp->matcher, view, iapp);
	}

	g_list_free(windows);

	return;
}

/* When new windows are born, we check to see if they're desktop
   windows. */
static void
new_window (BamfMatcher * matcher, BamfView * view, gpointer user_data)
{
	BamfWindow * window = BAMF_WINDOW(view);
	if (window == NULL) {
		return;
	}

	if (bamf_window_get_window_type(window) != BAMF_WINDOW_DESKTOP) {
		return;
	}

	IndicatorAppmenu * iapp = INDICATOR_APPMENU(user_data);
	guint xid = bamf_window_get_xid(window);
	g_hash_table_insert(iapp->desktop_windows, GUINT_TO_POINTER(xid), GINT_TO_POINTER(TRUE));

	g_debug("New Desktop Window: %X", xid);

	gpointer pwm = g_hash_table_lookup(iapp->apps, GUINT_TO_POINTER(xid));
	if (pwm != NULL) {
		WindowMenus * wm = WINDOW_MENUS(pwm);
		iapp->desktop_menu = wm;
		g_debug("Setting Desktop Menus to: %X", xid);
		if (iapp->active_window == NULL && iapp->default_app == NULL) {
			switch_default_app(iapp, NULL, NULL);
		}
	}

	return;
}

/* When windows leave us, this function gets called */
static void
old_window (BamfMatcher * matcher, BamfView * view, gpointer user_data)
{
	BamfWindow * window = BAMF_WINDOW(view);
	if (window == NULL) {
		return;
	}

	if (bamf_window_get_window_type(window) != BAMF_WINDOW_DESKTOP) {
		return;
	}

	IndicatorAppmenu * iapp = INDICATOR_APPMENU(user_data);
	g_hash_table_remove(iapp->desktop_windows, GUINT_TO_POINTER(bamf_window_get_xid(window)));

	return;
}

/* List of desktop files that shouldn't have menu stubs. */
const static gchar * stubs_blacklist[] = {
	/* Firefox */
	"/usr/share/applications/firefox.desktop",
	/* Thunderbird */
	"/usr/share/applications/thunderbird.desktop",
	/* Open Office */
	"/usr/share/applications/openoffice.org-base.desktop",
	"/usr/share/applications/openoffice.org-impress.desktop",
	"/usr/share/applications/openoffice.org-calc.desktop",
	"/usr/share/applications/openoffice.org-math.desktop",
	"/usr/share/applications/openoffice.org-draw.desktop",
	"/usr/share/applications/openoffice.org-writer.desktop",
	/* Blender */
	"/usr/share/applications/blender-fullscreen.desktop",
	"/usr/share/applications/blender-windowed.desktop",
	/* Eclipse */
	"/usr/share/applications/eclipse.desktop",

	NULL
};

/* Check with BAMF, and then check the blacklist of desktop files
   to see if any are there.  Otherwise, show the stubs. */
gboolean
show_menu_stubs (BamfApplication * app)
{
	if (bamf_application_get_show_menu_stubs(app) == FALSE) {
		return FALSE;
	}

	const gchar * desktop_file = bamf_application_get_desktop_file(app);
	if (desktop_file == NULL || desktop_file[0] == '\0') {
		return TRUE;
	}

	int i;
	for (i = 0; stubs_blacklist[i] != NULL; i++) {
		if (g_strcmp0(stubs_blacklist[i], desktop_file) == 0) {
			return FALSE;
		}
	}

	return TRUE;
}

/* Get the current set of entries */
static GList *
get_entries (IndicatorObject * io)
{
	g_return_val_if_fail(IS_INDICATOR_APPMENU(io), NULL);
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(io);

	/* If we have a focused app with menus, use it's windows */
	if (iapp->default_app != NULL) {
		return window_menus_get_entries(iapp->default_app);
	}

	/* Else, let's go with desktop windows if there isn't a focused window */
	if (iapp->active_window == NULL) {
		if (iapp->desktop_menu == NULL) {
			return NULL;
		} else {
			return window_menus_get_entries(iapp->desktop_menu);
		}
	}

	/* Oh, now we're looking at stubs. */

	if (iapp->active_stubs == STUBS_UNKNOWN) {
		iapp->active_stubs = STUBS_SHOW;

		BamfApplication * app = bamf_matcher_get_application_for_window(iapp->matcher, iapp->active_window);
		if (app != NULL) {
			/* First check to see if we can find an app, then if we can
			   check to see if it has an opinion on whether we should
			   show the stubs or not. */
			if (show_menu_stubs(app) == FALSE) {
				/* If it blocks them, fall out. */
				iapp->active_stubs = STUBS_HIDE;
			}
		}
	}

	if (iapp->active_stubs == STUBS_HIDE) {
		return NULL;
	}

	GList * output = NULL;
	int i;

	/* There is only one item in window_menus now, but there
	   was more, and there is likely to be more in the future
	   so we're leaving this here to avoid a possible bug. */
	for (i = 0; i < iapp->window_menus->len; i++) {
		output = g_list_append(output, &g_array_index(iapp->window_menus, IndicatorObjectEntry, i));
	}

	return output;
}

/* Grabs the location of the entry */
static guint
get_location (IndicatorObject * io, IndicatorObjectEntry * entry)
{
	guint count = 0;
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(io);
	if (iapp->default_app != NULL) {
		/* Find the location in the app */
		count = window_menus_get_location(iapp->default_app, entry);
	} else if (iapp->active_window != NULL) {
		/* Find the location in the window menus */
		for (count = 0; count < iapp->window_menus->len; count++) {
			if (entry == &g_array_index(iapp->window_menus, IndicatorObjectEntry, count)) {
				break;
			}
		}
		if (count == iapp->window_menus->len) {
			g_warning("Unable to find entry in default window menus");
			count = 0;
		}
	} else {
		/* Find the location in the desktop menu */
		if (iapp->desktop_menu != NULL) {
			count = window_menus_get_location(iapp->desktop_menu, entry);
		}
	}
	return count;
}

/* Responds to a menuitem being activated on the panel. */
static void
entry_activate (IndicatorObject * io, IndicatorObjectEntry * entry, guint timestamp)
{
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(io);

	if (iapp->default_app != NULL) {
		window_menus_entry_activate(iapp->default_app, entry, timestamp);
		return;
	}

	if (iapp->active_window == NULL) {
		if (iapp->desktop_menu != NULL) {
			window_menus_entry_activate(iapp->desktop_menu, entry, timestamp);
		}
		return;
	}

	/* Else we've got stubs, and the stubs don't care. */

	return;
}

/* Checks to see we cared about a window that's going
   away, so that we can deal with that */
static void
window_finalized_is_active (gpointer user_data, GObject * old_window)
{
	g_return_if_fail(IS_INDICATOR_APPMENU(user_data));
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(user_data);

	/* Pointer comparison as we can't really trust any of the
	   pointers to do any dereferencing */
	if ((gpointer)iapp->active_window != (gpointer)old_window) {
		/* Ah, no issue, we weren't caring about this one
		   anyway. */
		return;
	}

	/* We're going to a state where we don't know what the active
	   window is, hopefully BAMF will save us */
	active_window_changed (iapp->matcher, NULL, NULL, iapp);

	return;
}

/* A helper for switch_default_app that takes care of the
   switching of the active window variable */
static void
switch_active_window (IndicatorAppmenu * iapp, BamfWindow * active_window)
{
	if (iapp->active_window == active_window) {
		return;
	}

	if (iapp->active_window != NULL) {
		g_object_weak_unref(G_OBJECT(iapp->active_window), window_finalized_is_active, iapp);
	}

	iapp->active_window = active_window;
	iapp->active_stubs = STUBS_UNKNOWN;

	if (active_window != NULL) {
		g_object_weak_ref(G_OBJECT(active_window), window_finalized_is_active, iapp);
	}

	if (iapp->close_item == NULL) {
		g_warning("No close item!?!?!");
		return;
	}

	gtk_widget_set_sensitive(GTK_WIDGET(iapp->close_item), FALSE);

	if (iapp->active_window == NULL) {
		return;
	}

	guint xid = bamf_window_get_xid(iapp->active_window);
	if (xid == 0 || bamf_view_is_closed (BAMF_VIEW (iapp->active_window))) {
		return;
	}
 
	GdkWMFunction functions;
	if (!egg_xid_get_functions(xid, &functions)) {
		g_debug("Unable to get MWM functions for: %d", xid);
		functions = GDK_FUNC_ALL;
	}

	if (functions & GDK_FUNC_ALL || functions & GDK_FUNC_CLOSE) {
		gtk_widget_set_sensitive(GTK_WIDGET(iapp->close_item), TRUE);
	}

	return;
}

/* Switch applications, remove all the entires for the previous
   one and add them for the new application */
static void
switch_default_app (IndicatorAppmenu * iapp, WindowMenus * newdef, BamfWindow * active_window)
{
	GList * entry_head, * entries;

	if (iapp->default_app == newdef && iapp->default_app != NULL) {
		/* We've got an app with menus and it hasn't changed. */

		/* Keep active window up-to-date, though we're probably not
		   using it much. */
		switch_active_window(iapp, active_window);
		return;
	}

	if (iapp->default_app == NULL && iapp->active_window == active_window && newdef == NULL) {
		/* There's no application menus, but the active window hasn't
		   changed.  So there's no change. */
		return;
	}

	entry_head = indicator_object_get_entries(INDICATOR_OBJECT(iapp));


	for (entries = entry_head; entries != NULL; entries = g_list_next(entries)) {
		IndicatorObjectEntry * entry = (IndicatorObjectEntry *)entries->data;

		if (entry->label != NULL) {
			gtk_widget_hide(GTK_WIDGET(entry->label));
		}

		if (entry->menu != NULL) {
			gtk_menu_detach(entry->menu);
		}

		g_signal_emit(G_OBJECT(iapp), INDICATOR_OBJECT_SIGNAL_ENTRY_REMOVED_ID, 0, entries->data, TRUE);
	}

	g_list_free(entry_head);

	
	/* Disconnect signals */
	if (iapp->sig_entry_added != 0) {
		g_signal_handler_disconnect(G_OBJECT(iapp->default_app), iapp->sig_entry_added);
		iapp->sig_entry_added = 0;
	}
	if (iapp->sig_entry_removed != 0) {
		g_signal_handler_disconnect(G_OBJECT(iapp->default_app), iapp->sig_entry_removed);
		iapp->sig_entry_removed = 0;
	}
	if (iapp->sig_show_menu != 0) {
		g_signal_handler_disconnect(G_OBJECT(iapp->default_app), iapp->sig_show_menu);
		iapp->sig_show_menu = 0;
	}

	/* Default App is NULL, let's see if it needs replacement */
	iapp->default_app = NULL;

	/* Update the active window pointer -- may be NULL */
	switch_active_window(iapp, active_window);

	/* If we're putting up a new window, let's do that now. */
	if (newdef != NULL) {
		/* Switch */
		iapp->default_app = newdef;

		/* Connect signals */
		iapp->sig_entry_added =   g_signal_connect(G_OBJECT(iapp->default_app),
		                                           WINDOW_MENUS_SIGNAL_ENTRY_ADDED,
		                                           G_CALLBACK(window_entry_added),
		                                           iapp);
		iapp->sig_entry_removed = g_signal_connect(G_OBJECT(iapp->default_app),
		                                           WINDOW_MENUS_SIGNAL_ENTRY_REMOVED,
		                                           G_CALLBACK(window_entry_removed),
		                                           iapp);
		iapp->sig_show_menu     = g_signal_connect(G_OBJECT(iapp->default_app),
		                                           WINDOW_MENUS_SIGNAL_SHOW_MENU,
		                                           G_CALLBACK(window_show_menu),
		                                           iapp);
	}

	/* Get our new list of entries.  Now we can go ahead and signal
	   that each of them has been added */

	entry_head = indicator_object_get_entries(INDICATOR_OBJECT(iapp));

	for (entries = entry_head; entries != NULL; entries = g_list_next(entries)) {
		IndicatorObjectEntry * entry = (IndicatorObjectEntry *)entries->data;

		if (iapp->default_app != NULL) {
			window_menus_entry_restore(iapp->default_app, entry);
		} else {
			if (iapp->active_window == NULL) {
				/* Desktop Menus */
				window_menus_entry_restore(iapp->desktop_menu, entry);
			} else {
				/* Window Menus */
				if (entry->label != NULL) {
					gtk_widget_show(GTK_WIDGET(entry->label));
				}
			}
		}

		g_signal_emit(G_OBJECT(iapp), INDICATOR_OBJECT_SIGNAL_ENTRY_ADDED_ID, 0, entries->data, TRUE);
	}

	g_list_free(entry_head);

	return;
}

/* Recieve the signal that the window being shown
   has now changed. */
static void
active_window_changed (BamfMatcher * matcher, BamfView * oldview, BamfView * newview, gpointer user_data)
{
	BamfWindow * window = NULL;

	if (newview != NULL) {
		window = BAMF_WINDOW(newview);
		if (window == NULL) {
			g_warning("Active window changed to View thats not a window.");
		}
	} else {
		g_debug("Active window is: NULL");
	}

	IndicatorAppmenu * appmenu = INDICATOR_APPMENU(user_data);

	if (window != NULL && bamf_window_get_window_type(window) == BAMF_WINDOW_DESKTOP) {
		g_debug("Switching to menus from desktop");
		switch_default_app(appmenu, NULL, NULL);
		return;
	}

	WindowMenus * menus = NULL;
	guint32 xid = 0;

	while (window != NULL && menus == NULL) {
		xid = bamf_window_get_xid(window);
	
		menus = g_hash_table_lookup(appmenu->apps, GUINT_TO_POINTER(xid));

		if (menus == NULL) {
			g_debug("Looking for parent window on XID %d", xid);
			window = bamf_window_get_transient(window);
		}
	}

	/* Note: We're not using window here, but re-casting the
	   newwindow variable.  Which means we stay where we were
	   but get the menus from parents. */
	g_debug("Switching to menus from XID %d", xid);
	if (newview != NULL) {
		switch_default_app(appmenu, menus, BAMF_WINDOW(newview));
	} else {
		switch_default_app(appmenu, menus, NULL);
	}

	return;
}

/* Respond to the menus being destroyed.  We need to deregister
   and make sure we weren't being shown.  */
static void
menus_destroyed (GObject * menus, gpointer user_data)
{
	gboolean reload_menus = FALSE;
	WindowMenus * wm = WINDOW_MENUS(menus);
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(user_data);

	guint xid = window_menus_get_xid(wm);
	g_return_if_fail(xid != 0);

	g_hash_table_steal(iapp->apps, GUINT_TO_POINTER(xid));

	g_debug("Removing menus for %d", xid);

	if (iapp->desktop_menu == wm) {
		iapp->desktop_menu = NULL;
		determine_new_desktop(iapp);
		if (iapp->default_app == NULL && iapp->active_window == NULL) {
			reload_menus = TRUE;
		}
	}

	/* If we're it, let's remove ourselves and BAMF will probably
	   give us a new entry in a bit. */
	if (iapp->default_app == wm) {
		reload_menus = TRUE;
	}

	if (reload_menus) {
		switch_default_app(iapp, NULL, NULL);
	}

	return;
}

/* A new window wishes to register it's windows with us */
static GVariant *
register_window (IndicatorAppmenu * iapp, guint windowid, const gchar * objectpath,
                 const gchar * sender)
{
	g_debug("Registering window ID %d with path %s from %s", windowid, objectpath, sender);

	if (g_hash_table_lookup(iapp->apps, GUINT_TO_POINTER(windowid)) == NULL && windowid != 0) {
		WindowMenus * wm = window_menus_new(windowid, sender, objectpath);
		g_return_val_if_fail(wm != NULL, FALSE);

		g_signal_connect(G_OBJECT(wm), WINDOW_MENUS_SIGNAL_DESTROY, G_CALLBACK(menus_destroyed), iapp);

		g_hash_table_insert(iapp->apps, GUINT_TO_POINTER(windowid), wm);

		emit_signal(iapp, "WindowRegistered",
		            g_variant_new("(uso)", windowid, sender, objectpath));

		gpointer pdesktop = g_hash_table_lookup(iapp->desktop_windows, GUINT_TO_POINTER(windowid));
		if (pdesktop != NULL) {
			determine_new_desktop(iapp);
		}

		/* Note: Does not cause ref */
		BamfWindow * win = bamf_matcher_get_active_window(iapp->matcher);

		active_window_changed(iapp->matcher, NULL, BAMF_VIEW(win), iapp);
	} else {
		if (windowid == 0) {
			g_warning("Can't build windows for a NULL window ID %d with path %s from %s", windowid, objectpath, sender);
		} else {
			g_warning("Already have a menu for window ID %d with path %s from %s", windowid, objectpath, sender);
		}
	}

	return g_variant_new("()");
}

/* Kindly remove an entry from our DB */
static GVariant *
unregister_window (IndicatorAppmenu * iapp, guint windowid)
{
	/* TODO: Do it */

	return g_variant_new("()");
}

/* Grab the menu information for a specific window */
static GVariant *
get_menu_for_window (IndicatorAppmenu * iapp, guint windowid, GError ** error)
{
	WindowMenus * wm = NULL;

	if (windowid == 0) {
		wm = iapp->default_app;
	} else {
		wm = WINDOW_MENUS(g_hash_table_lookup(iapp->apps, GUINT_TO_POINTER(windowid)));
	}

	if (wm == NULL) {
		g_set_error_literal(error, error_quark(), ERROR_WINDOW_NOT_FOUND, "Window not found");
		return NULL;
	}

	return g_variant_new("(so)", window_menus_get_address(wm),
	                     window_menus_get_path(wm));
}

/* Get all the menus we have */
static GVariant *
get_menus (IndicatorAppmenu * iapp, GError ** error)
{
	if (iapp->apps == NULL) {
		g_set_error_literal(error, error_quark(), ERROR_NO_APPLICATIONS, "No applications are registered");
		return NULL;
	}

	GVariantBuilder * builder = g_variant_builder_new (G_VARIANT_TYPE_ARRAY);
	GList * appkeys = NULL;
	for (appkeys = g_hash_table_get_keys(iapp->apps); appkeys != NULL; appkeys = g_list_next(appkeys)) {
		gpointer hash_val = g_hash_table_lookup(iapp->apps, appkeys->data);

		if (hash_val == NULL) { continue; }

		g_variant_builder_add (builder, "(uso)",
		                       window_menus_get_xid(WINDOW_MENUS(hash_val)),
		                       window_menus_get_path(WINDOW_MENUS(hash_val)),
		                       window_menus_get_address(WINDOW_MENUS(hash_val)));
	}

	return g_variant_new("(a(uso))", builder);
}

/* A method has been called from our dbus inteface.  Figure out what it
   is and dispatch it. */
static void
bus_method_call (GDBusConnection * connection, const gchar * sender,
                 const gchar * path, const gchar * interface,
                 const gchar * method, GVariant * params,
                 GDBusMethodInvocation * invocation, gpointer user_data)
{
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(user_data);
	GVariant * retval = NULL;
	GError * error = NULL;

	if (g_strcmp0(method, "RegisterWindow") == 0) {
		guint32 xid;
		const gchar * path;
		g_variant_get(params, "(u&o)", &xid, &path);
		retval = register_window(iapp, xid, path, sender);
	} else if (g_strcmp0(method, "UnregisterWindow") == 0) {
		guint32 xid;
		g_variant_get(params, "(u)", &xid);
		retval = unregister_window(iapp, xid);
	} else if (g_strcmp0(method, "GetMenuForWindow") == 0) {
		guint32 xid;
		g_variant_get(params, "(u)", &xid);
		retval = get_menu_for_window(iapp, xid, &error);
	} else if (g_strcmp0(method, "GetMenus") == 0) {
		retval = get_menus(iapp, &error);
	} else {
		g_warning("Calling method '%s' on the indicator service and it's unknown", method);
	}

	if (error != NULL) {
		g_dbus_method_invocation_return_dbus_error(invocation,
		                                           "org.ayatana.AppMenu.Error",
		                                           error->message);
		g_error_free(error);
	} else {
		g_dbus_method_invocation_return_value(invocation, retval);
	}
	return;
}

/* Pass up the entry added event */
static void
window_entry_added (WindowMenus * mw, IndicatorObjectEntry * entry, gpointer user_data)
{
	g_signal_emit_by_name(G_OBJECT(user_data), INDICATOR_OBJECT_SIGNAL_ENTRY_ADDED, entry);
	return;
}

/* Pass up the entry removed event */
static void
window_entry_removed (WindowMenus * mw, IndicatorObjectEntry * entry, gpointer user_data)
{
	g_signal_emit_by_name(G_OBJECT(user_data), INDICATOR_OBJECT_SIGNAL_ENTRY_REMOVED, entry);
	return;
}

/* Pass up the show menu event */
static void
window_show_menu (WindowMenus * mw, IndicatorObjectEntry * entry, guint timestamp, gpointer user_data)
{
	g_signal_emit_by_name(G_OBJECT(user_data), INDICATOR_OBJECT_SIGNAL_MENU_SHOW, entry, timestamp);
	return;
}

/**********************
  DEBUG INTERFACE
 **********************/

/* Builds the error quark if we need it, otherwise just
   returns the same value */
static GQuark
error_quark (void)
{
	static GQuark error_quark = 0;

	if (error_quark == 0) {
		error_quark = g_quark_from_static_string("indicator-appmenu");
	}

	return error_quark;
}

/* Looks to see if we can find an accel label to steal the
   closure from */
static void
find_closure (GtkWidget * widget, gpointer user_data)
{
	GClosure ** closure = (GClosure **)user_data;

	/* If we have one quit */
	if (*closure != NULL) {
		return;
	}
	
	/* If we've got a label, steal its */
	if (GTK_IS_ACCEL_LABEL(widget)) {
		g_object_get (widget,
		              "accel-closure", closure,
		              NULL);
		return;
	}

	/* If we've got a container, dig deeper */
	if (GTK_IS_CONTAINER(widget)) {
		gtk_container_foreach(GTK_CONTAINER(widget), find_closure, user_data);
	}

	return;
}

/* Look at the closures in an accel group and find
   the one that matches the one we've been passed */
static gboolean
find_group_closure (GtkAccelKey * key, GClosure * closure, gpointer user_data)
{
	return closure == user_data;
}

/* Turn the key codes into a string for the JSON output */
static void
key2string (GtkAccelKey * key, GArray * strings)
{
	gchar * temp = NULL;

	temp = g_strdup(", \"" DBUSMENU_MENUITEM_PROP_SHORTCUT "\": [[");
	g_array_append_val(strings, temp);

	if (key->accel_mods & GDK_CONTROL_MASK) {
		temp = g_strdup_printf("\"%s\", ", DBUSMENU_MENUITEM_SHORTCUT_CONTROL);
		g_array_append_val(strings, temp);
	}
	if (key->accel_mods & GDK_MOD1_MASK) {
		temp = g_strdup_printf("\"%s\", ", DBUSMENU_MENUITEM_SHORTCUT_ALT);
		g_array_append_val(strings, temp);
	}
	if (key->accel_mods & GDK_SHIFT_MASK) {
		temp = g_strdup_printf("\"%s\", ", DBUSMENU_MENUITEM_SHORTCUT_SHIFT);
		g_array_append_val(strings, temp);
	}
	if (key->accel_mods & GDK_SUPER_MASK) {
		temp = g_strdup_printf("\"%s\", ", DBUSMENU_MENUITEM_SHORTCUT_SUPER);
		g_array_append_val(strings, temp);
	}

	temp = g_strdup_printf("\"%s\"]]", gdk_keyval_name(key->accel_key));
	g_array_append_val(strings, temp);

	return;
}

/* Do something for each menu item */
static void
menu_iterator (GtkWidget * widget, gpointer user_data)
{
	GArray * strings = (GArray *)user_data;

	gchar * temp = g_strdup("{");
	g_array_append_val(strings, temp);

	/* TODO: We need some sort of useful ID, but for now  we're
	   just ensuring it's unique. */
	temp = g_strdup_printf("\"id\": %d", strings->len);
	g_array_append_val(strings, temp);

	if (GTK_IS_SEPARATOR_MENU_ITEM(widget)) {
		temp = g_strdup_printf(", \"" DBUSMENU_MENUITEM_PROP_TYPE "\": \"%s\"", DBUSMENU_CLIENT_TYPES_SEPARATOR);
		g_array_append_val(strings, temp);
	} else {
		temp = g_strdup_printf(", \"" DBUSMENU_MENUITEM_PROP_TYPE "\": \"%s\"", DBUSMENU_CLIENT_TYPES_DEFAULT);
		g_array_append_val(strings, temp);
	}

	temp = g_strdup_printf(", \"" DBUSMENU_MENUITEM_PROP_ENABLED "\": %s", gtk_widget_get_sensitive(GTK_WIDGET(widget)) ? "true" : "false");
	g_array_append_val(strings, temp);

	temp = g_strdup_printf(", \"" DBUSMENU_MENUITEM_PROP_VISIBLE "\": %s", gtk_widget_get_visible(GTK_WIDGET(widget)) ? "true" : "false");
	g_array_append_val(strings, temp);

	const gchar * label = gtk_menu_item_get_label(GTK_MENU_ITEM(widget));
	if (label != NULL) {
		temp = g_strdup_printf(", \"" DBUSMENU_MENUITEM_PROP_LABEL "\": \"%s\"", label);
		g_array_append_val(strings, temp);
	}

	/* Deal with shortcuts, find the accel closure if we can and then
	   turn that into a string */
	GClosure * closure = NULL;
	find_closure(widget, &closure);

	if (closure != NULL) {
		GtkAccelGroup * group = gtk_accel_group_from_accel_closure(closure);
		if (group != NULL) {
			GtkAccelKey * key = gtk_accel_group_find(group, find_group_closure, closure);
			if (key != NULL) {
				key2string(key, strings);
			}
		}
	}

	/* TODO: Handle check/radio items */

	GtkWidget * submenu = gtk_menu_item_get_submenu(GTK_MENU_ITEM(widget));
	if (submenu != NULL) {
		temp = g_strdup(", \"" DBUSMENU_MENUITEM_PROP_CHILD_DISPLAY "\": \"" DBUSMENU_MENUITEM_CHILD_DISPLAY_SUBMENU "\"");
		g_array_append_val(strings, temp);

		temp = g_strdup(", \"submenu\": [ ");
		g_array_append_val(strings, temp);

		guint old_len = strings->len;

		gtk_container_foreach(GTK_CONTAINER(submenu), menu_iterator, strings);

		if (old_len == strings->len) {
			temp = g_strdup("]");
			g_array_append_val(strings, temp);
		} else {
			gchar * last_one = g_array_index(strings, gchar *, strings->len - 1);
			guint lastlen = g_utf8_strlen(last_one, -1);

			if (last_one[lastlen - 1] != ',') {
				g_warning("Huh, this seems impossible.  Should be a comma at the end.");
			}

			last_one[lastlen - 1] = ']';
		}
	}

	temp = g_strdup("},");
	g_array_append_val(strings, temp);

	return;
}

/* Takes an entry and outputs it into the ptrarray */
static void
entry2json (IndicatorObjectEntry * entry, GArray * strings)
{
	gchar * temp = g_strdup("{");
	g_array_append_val(strings, temp);

	/* TODO: We need some sort of useful ID, but for now  we're
	   just ensuring it's unique. */
	temp = g_strdup_printf("\"id\": %d", strings->len);
	g_array_append_val(strings, temp);

	if (entry->label != NULL) {
		temp = g_strdup_printf(", \"" DBUSMENU_MENUITEM_PROP_LABEL "\": \"%s\"", gtk_label_get_label(entry->label));
		g_array_append_val(strings, temp);

		temp = g_strdup_printf(", \"" DBUSMENU_MENUITEM_PROP_ENABLED "\": %s", gtk_widget_get_sensitive(GTK_WIDGET(entry->label)) ? "true" : "false");
		g_array_append_val(strings, temp);

		temp = g_strdup_printf(", \"" DBUSMENU_MENUITEM_PROP_VISIBLE "\": %s", gtk_widget_get_visible(GTK_WIDGET(entry->label)) ? "true" : "false");
		g_array_append_val(strings, temp);
	}

	if (entry->menu != NULL) {
		temp = g_strdup(", \"" DBUSMENU_MENUITEM_PROP_CHILD_DISPLAY "\": \"" DBUSMENU_MENUITEM_CHILD_DISPLAY_SUBMENU "\"");
		g_array_append_val(strings, temp);

		temp = g_strdup(", \"submenu\": [ ");
		g_array_append_val(strings, temp);

		guint old_len = strings->len;

		gtk_container_foreach(GTK_CONTAINER(entry->menu), menu_iterator, strings);

		if (old_len == strings->len) {
			temp = g_strdup("]");
			g_array_append_val(strings, temp);
		} else {
			gchar * last_one = g_array_index(strings, gchar *, strings->len - 1);
			guint lastlen = g_utf8_strlen(last_one, -1);

			if (last_one[lastlen - 1] != ',') {
				g_warning("Huh, this seems impossible.  Should be a comma at the end.");
			}

			last_one[lastlen - 1] = ']';
		}
	}


	temp = g_strdup("},");
	g_array_append_val(strings, temp);

	return;
}

/* Grab the location of the dbusmenu of the current menu */
static GVariant *
get_current_menu (IndicatorAppmenuDebug * iappd, GError ** error)
{
	IndicatorAppmenu * iapp = iappd->appmenu;

	if (iapp->default_app == NULL) {
		g_set_error_literal(error, error_quark(), ERROR_NO_DEFAULT_APP, "Not currently showing an application");
		return NULL;
	}

	return g_variant_new("(so)", window_menus_get_address(iapp->default_app),
	                     window_menus_get_path(iapp->default_app));
}

/* Activate menu items through a script given as a parameter */
static GVariant *
activate_menu_item (IndicatorAppmenuDebug * iappd, GVariantIter * iter, GError ** error)
{
	/* TODO: Do it */

	return g_variant_new("()");
}

/* Dump a specific window's menus to a JSON file */
static GVariant *
dump_menu (IndicatorAppmenuDebug * iappd, guint windowid, GError ** error)
{
	IndicatorAppmenu * iapp = iappd->appmenu;
	WindowMenus * wm = NULL;
	gchar * jsondata = NULL;

	if (windowid == 0) {
		wm = iapp->default_app;
	} else {
		wm = WINDOW_MENUS(g_hash_table_lookup(iapp->apps, GUINT_TO_POINTER(windowid)));
	}

	if (wm == NULL) {
		g_set_error_literal(error, error_quark(), ERROR_WINDOW_NOT_FOUND, "Window not found");
		return NULL;
	}

	GArray * strings = g_array_new(TRUE, FALSE, sizeof(gchar *));
	GList * entries = window_menus_get_entries(wm);
	GList * entry;

	gchar * temp = g_strdup("{");
	g_array_append_val(strings, temp);

	temp = g_strdup("\"id\": 0");
	g_array_append_val(strings, temp);

	if (entries != NULL) {
		temp = g_strdup(", \"" DBUSMENU_MENUITEM_PROP_CHILD_DISPLAY "\": \"" DBUSMENU_MENUITEM_CHILD_DISPLAY_SUBMENU "\"");
		g_array_append_val(strings, temp);

		temp = g_strdup(", \"submenu\": [");
		g_array_append_val(strings, temp);
	}

	guint old_len = strings->len;

	for (entry = entries; entry != NULL; entry = g_list_next(entry)) {
		entry2json(entry->data, strings);
	}

	if (entries != NULL) {
		if (old_len == strings->len) {
			temp = g_strdup("]");
			g_array_append_val(strings, temp);
		} else {
			gchar * last_one = g_array_index(strings, gchar *, strings->len - 1);
			guint lastlen = g_utf8_strlen(last_one, -1);

			if (last_one[lastlen - 1] != ',') {
				g_warning("Huh, this seems impossible.  Should be a comma at the end.");
			}

			last_one[lastlen - 1] = ']';
		}
	}

	g_list_free(entries);

	temp = g_strdup("}");
	g_array_append_val(strings, temp);

	jsondata = g_strjoinv(NULL, (gchar **)strings->data);
	g_strfreev((gchar **)strings->data);
	g_array_free(strings, FALSE);

	GVariant * rv = g_variant_new("(s)", jsondata);
	g_free(jsondata);
	return rv;
}

/* Dump the current menu to a JSON file */
static GVariant *
dump_current_menu  (IndicatorAppmenuDebug * iappd, GError ** error)
{
	return dump_menu(iappd, 0, error);
}

/* A method has been called from our dbus inteface.  Figure out what it
   is and dispatch it. */
static void
dbg_bus_method_call (GDBusConnection * connection, const gchar * sender,
                     const gchar * path, const gchar * interface,
                     const gchar * method, GVariant * params,
                     GDBusMethodInvocation * invocation, gpointer user_data)
{
	IndicatorAppmenuDebug * iappd = INDICATOR_APPMENU_DEBUG(user_data);
	GVariant * retval = NULL;
	GError * error = NULL;

	if (g_strcmp0(method, "GetCurrentMenu") == 0) {
		retval = get_current_menu(iappd, &error);
	} else if (g_strcmp0(method, "ActivateMenuItem") == 0) {
		GVariantIter *iter;
		g_variant_get(params, "(ai)", &iter);
		retval = activate_menu_item(iappd, iter, &error);
		g_variant_iter_free(iter);
	} else if (g_strcmp0(method, "DumpCurrentMenu") == 0) {
		retval = dump_current_menu(iappd, &error);
	} else if (g_strcmp0(method, "DumpMenu") == 0) {
		guint32 xid;
		g_variant_get(params, "(u)", &xid);
		retval = dump_menu(iappd, xid, &error);
	} else {
		g_warning("Calling method '%s' on the indicator service and it's unknown", method);
	}

	if (error != NULL) {
		g_dbus_method_invocation_return_dbus_error(invocation,
		                                           "org.ayatana.AppMenu.Error",
		                                           error->message);
		g_error_free(error);
	} else {
		g_dbus_method_invocation_return_value(invocation, retval);
	}
	return;
}

