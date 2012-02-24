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
#include <libindicator/indicator-image-helper.h>

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

typedef enum _MenuMode MenuMode;
enum _MenuMode {
	MENU_MODE_SEVERAL,
	MENU_MODE_SINGLE
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

	gulong sig_status_changed;
	gulong sig_show_menu;
	gulong sig_menu_changed;

	GtkMenuItem * close_item;

	GArray * window_menus;
	GList * application_menus;

	GHashTable * desktop_windows;
	WindowMenus * desktop_menu;

	GDBusConnection * bus;
	guint owner_id;
	guint dbus_registration;

	GHashTable * destruction_timers;

	GSettings * settings;
	MenuMode menu_mode;
	IndicatorObjectEntry single_menu;
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
static void indicator_appmenu_dispose                                (GObject *object);
static void indicator_appmenu_finalize                               (GObject *object);
static void build_window_menus                                       (IndicatorAppmenu * iapp);
static GList * get_entries                                           (IndicatorObject * io);
static guint get_location                                            (IndicatorObject * io,
                                                                      IndicatorObjectEntry * entry);
static void entry_activate                                           (IndicatorObject * io,
                                                                      IndicatorObjectEntry * entry,
                                                                      guint timestamp);
static void entry_activate_window                                    (IndicatorObject * io,
                                                                      IndicatorObjectEntry * entry,
                                                                      guint windowid,
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
static void window_status_changed                                    (WindowMenus * mw,
                                                                      DbusmenuStatus status,
                                                                      IndicatorAppmenu * iapp);
static void single_show_menu                                         (WindowMenus * mw,
                                                                      GtkMenuItem * item,
                                                                      guint timestamp,
                                                                      gpointer user_data);
static void window_show_menu                                         (WindowMenus * mw,
                                                                      GtkMenuItem * item,
                                                                      guint timestamp,
                                                                      gpointer user_data);
static void window_menu_changed                                      (WindowMenus * mw,
                                                                      gpointer user_data);
static void active_window_changed                                    (BamfMatcher * matcher,
                                                                      BamfView * oldview,
                                                                      BamfView * newview,
                                                                      gpointer user_data);
static void menu_mode_changed                                        (GSettings * settings,
                                                                      const gchar * key,
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
static void menus_destroyed                                          (GObject * menus,
                                                                      gpointer user_data);
static void source_unregister                                        (gpointer user_data);
static GVariant * unregister_window                                  (IndicatorAppmenu * iapp,
                                                                      guint windowid);
static gboolean settings_schema_exists                               (const gchar * schema);
GtkMenu * get_current_menu                                           (IndicatorAppmenu * iapp);
static void sync_menu_to_app_entries                                 (IndicatorAppmenu * iapp,
                                                                      GtkMenu * menu);
static void remove_entry                                             (IndicatorAppmenu * iapp,
                                                                      GList * item);

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
	ioclass->entry_activate_window = entry_activate_window;

	ioclass->entry_being_removed = NULL;
	ioclass->entry_was_added = NULL;

	/* Setting up the DBus interfaces */
	if (node_info == NULL) {
		GError * error = NULL;

		node_info = g_dbus_node_info_new_for_xml(_application_menu_registrar, &error);
		if (error != NULL) {
			g_critical("Unable to parse Application Menu Interface description: %s", error->message);
			g_error_free(error);
		}
	}

	if (interface_info == NULL) {
		interface_info = g_dbus_node_info_lookup_interface(node_info, REG_IFACE);

		if (interface_info == NULL) {
			g_critical("Unable to find interface '" REG_IFACE "'");
		}
	}

	return;
}

/* Per instance Init */
static void
indicator_appmenu_init (IndicatorAppmenu *self)
{
	self->default_app = NULL;
	self->apps = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_object_unref);
	self->matcher = NULL;
	self->active_window = NULL;
	self->active_stubs = STUBS_UNKNOWN;
	self->close_item = NULL;
	self->retry_registration = 0;
	self->bus = NULL;
	self->owner_id = 0;
	self->dbus_registration = 0;
	self->settings = NULL;
	self->menu_mode = MENU_MODE_SEVERAL;
	self->single_menu.parent_object = INDICATOR_OBJECT(self);
	self->single_menu.label = NULL;
	self->single_menu.image = NULL;
	self->single_menu.menu = NULL;
	self->single_menu.accessible_desc = NULL;
	self->single_menu.name_hint = "application-menus";

	/* Setup the entries for applications */
	self->application_menus = NULL;

	/* Setup the entries for the fallbacks */
	self->window_menus = g_array_sized_new(FALSE, FALSE, sizeof(IndicatorObjectEntry), 2);

	/* Getting our settings */
	if (settings_schema_exists("com.canonical.indicator.appmenu")) {
		self->settings = g_settings_new("com.canonical.indicator.appmenu");

		g_signal_connect(G_OBJECT(self->settings), "changed::menu-mode", G_CALLBACK(menu_mode_changed), self);
		menu_mode_changed(self->settings, "menu-mode", self);
	}

	/* Setup the cache of windows with possible desktop entries */
	self->desktop_windows = g_hash_table_new(g_direct_hash, g_direct_equal);
	self->desktop_menu = NULL; /* Starts NULL until found */

	/* Set up the hashtable of destruction timers */
	self->destruction_timers = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, source_unregister);

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

	return;
}

/* Check to see if a schema exists */
/* TODO: Use one in utils on merge */
static gboolean
settings_schema_exists (const gchar * schema)
{
	const gchar * const * schemas = g_settings_list_schemas();
	int i;

	for (i = 0; schemas[i] != NULL; i++) {
		if (g_strcmp0(schemas[i], schema) == 0) {
			return TRUE;
		}
	}

	return FALSE;
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
		g_critical("Unable to register the object to DBus: %s", error->message);
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
		g_critical("OMG! Unable to get a connection to DBus");
	}
	else {
		g_critical("Unable to claim the name %s", DBUS_NAME);
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

	while (iapp->application_menus != NULL) {
		remove_entry(iapp, iapp->application_menus);
	}

	if (iapp->destruction_timers != NULL) {
		/* These are in dispose and not finalize becuase the dereference
		   function removes timers that could need the object to be in
		   a valid state, so it's better to have them in dispose */
		g_hash_table_destroy(iapp->destruction_timers);
		iapp->destruction_timers = NULL;
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

	g_clear_object(&iapp->settings);

	g_clear_object(&iapp->single_menu.label);
	g_clear_object(&iapp->single_menu.image);
	g_clear_object(&iapp->single_menu.menu);
	if (iapp->single_menu.accessible_desc != NULL) {
		g_free((gchar *)iapp->single_menu.accessible_desc); /* cast to remove const */
		iapp->single_menu.accessible_desc = NULL;
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
		g_critical("Unable to send %s signal: %s", name, error->message);
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

	guint32 xid = bamf_window_get_xid(iapp->active_window);
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
#if GTK_CHECK_VERSION(3, 0, 0)
	gdk_error_trap_pop_ignored ();
#else
	gdk_error_trap_pop ();
#endif

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
	if (view == NULL || !BAMF_IS_WINDOW(view)) {
		return;
	}

	BamfWindow * window = BAMF_WINDOW(view);
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(user_data);
	guint32 xid = bamf_window_get_xid(window);

	/* Make sure we don't destroy it later */
	g_hash_table_remove(iapp->destruction_timers, GUINT_TO_POINTER(xid));

	if (bamf_window_get_window_type(window) != BAMF_WINDOW_DESKTOP) {
		return;
	}

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

/* Called to get a new menu mode from the settings */
static void
menu_mode_changed (GSettings * settings, const gchar * key, gpointer user_data)
{
	g_return_if_fail(IS_INDICATOR_APPMENU(user_data));
	g_return_if_fail(g_strcmp0(key, "menu-mode") == 0);

	IndicatorAppmenu * iapp = INDICATOR_APPMENU(user_data);
	MenuMode newmode = MENU_MODE_SEVERAL;
	gchar * strmode = g_settings_get_string(settings, key);

	/* GSettings represents the enum as a set of strings.  So we need
	   to convert that back into a real enum here */
	if (g_strcmp0(strmode, "global") == 0) {
		newmode = MENU_MODE_SEVERAL;
	} else if (g_strcmp0(strmode, "locally-integrated") == 0) {
		newmode = MENU_MODE_SINGLE;
	} else {
		g_assert_not_reached();
	}

	g_free(strmode);

	/* No change, we are done here */
	if (newmode == iapp->menu_mode) {
		return;
	}

	iapp->menu_mode = newmode;
	g_debug("Menu mode changed to: %d", iapp->menu_mode);

	/* If we're going to single menus make sure we have the objects
	   we need built */
	if (iapp->menu_mode == MENU_MODE_SINGLE) {
		if (iapp->single_menu.image == NULL) {
			iapp->single_menu.image = indicator_image_helper("indicator-appmenu-menu-panel");
			g_object_ref_sink(iapp->single_menu.image);
			gtk_widget_show(GTK_WIDGET(iapp->single_menu.image));
		}

		sync_menu_to_app_entries(iapp, NULL);

		iapp->single_menu.menu = get_current_menu(iapp);

		gtk_widget_set_sensitive(GTK_WIDGET(iapp->single_menu.image), iapp->single_menu.menu != NULL);
		g_signal_emit_by_name(G_OBJECT(iapp), INDICATOR_OBJECT_SIGNAL_ENTRY_ADDED, &(iapp->single_menu));

		if (iapp->default_app != NULL) {
			iapp->sig_show_menu     = g_signal_connect(G_OBJECT(iapp->default_app),
			                                           WINDOW_MENUS_SIGNAL_SHOW_MENU,
			                                           G_CALLBACK(single_show_menu),
			                                           iapp);
		}
	}

	if (iapp->menu_mode == MENU_MODE_SEVERAL) {
		g_signal_emit_by_name(G_OBJECT(iapp), INDICATOR_OBJECT_SIGNAL_ENTRY_REMOVED, &(iapp->single_menu));
		sync_menu_to_app_entries(iapp, get_current_menu(iapp));
	}

	return;
}

typedef struct _destroy_data_t destroy_data_t;
struct _destroy_data_t {
	IndicatorAppmenu * iapp;
	guint32 xid;
};

/* Timeout to finally cleanup the window.  Causes is to ignore glitches that
   come from BAMF/WNCK. */
static gboolean
destroy_window_timeout (gpointer user_data)
{
	destroy_data_t * destroy_data = (destroy_data_t *)user_data;
	g_hash_table_steal(destroy_data->iapp->destruction_timers, GUINT_TO_POINTER(destroy_data->xid));
	unregister_window(destroy_data->iapp, destroy_data->xid);
	return FALSE; /* free's data through source deregistration */
}

/* Unregisters the source in the hash table when it gets removed.  This ensure
   we don't leave any timeouts around */
static void
source_unregister (gpointer user_data)
{
	g_source_remove(GPOINTER_TO_UINT(user_data));
	return;
}

/* When windows leave us, this function gets called */
static void
old_window (BamfMatcher * matcher, BamfView * view, gpointer user_data)
{
	if (view == NULL || !BAMF_IS_WINDOW(view)) {
		return;
	}

	IndicatorAppmenu * iapp = INDICATOR_APPMENU(user_data);
	BamfWindow * window = BAMF_WINDOW(view);
	guint32 xid = bamf_window_get_xid(window);

	destroy_data_t * destroy_data = g_new0(destroy_data_t, 1);
	destroy_data->iapp = iapp;
	destroy_data->xid = xid;

	guint source_id = g_timeout_add_seconds_full(G_PRIORITY_LOW, 5, destroy_window_timeout, destroy_data, g_free);
	g_hash_table_replace(iapp->destruction_timers, GUINT_TO_POINTER(xid), GUINT_TO_POINTER(source_id));

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

/* Take a list of entries and turn it into a list instead of
   an array */
static GList *
entry_array_to_list (GArray * array)
{
	GList * output = NULL;
	int i;

	for (i = 0; i < array->len; i++) {
		output = g_list_prepend(output, &g_array_index(array, IndicatorObjectEntry, i));
	}

	return g_list_reverse(output);
}

/* Get the current set of entries */
static GList *
get_entries (IndicatorObject * io)
{
	g_return_val_if_fail(IS_INDICATOR_APPMENU(io), NULL);
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(io);

	/* If we're in single menu mode, just send that */
	if (iapp->menu_mode == MENU_MODE_SINGLE) {
		return g_list_append(NULL, &(iapp->single_menu));
	}

	/* If we have a focused app with menus, use it's windows */
	if (iapp->default_app != NULL) {
		return g_list_copy(iapp->application_menus);
	}

	/* Else, let's go with desktop windows if there isn't a focused window.
	   They should already be put in the application menus if that's the case */
	if (iapp->active_window == NULL) {
		return g_list_copy(iapp->application_menus);
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

	if (indicator_object_check_environment(INDICATOR_OBJECT(iapp), "unity")) {
		return NULL;
	}

	/* If we're down here we want to push out those stubs, let's
	   get 'em out! */
	return entry_array_to_list(iapp->window_menus);
}

/* Grabs the location of the entry */
static guint
get_location (IndicatorObject * io, IndicatorObjectEntry * entry)
{
	guint count = 0;
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(io);

	GArray * array = NULL;
	GList * list = NULL;

	if (iapp->default_app != NULL) {
		/* Find the location in the app */
		list = iapp->application_menus;
	} else if (iapp->active_window != NULL) {
		array = iapp->window_menus;
	} else {
		/* Find the location in the desktop menu */
		if (iapp->desktop_menu != NULL) {
			list = iapp->application_menus;
		}
	}

	/* Find the location in the array */
	if (array != NULL) {
		for (count = 0; count < array->len; count++) {
			if (entry == &g_array_index(array, IndicatorObjectEntry, count)) {
				break;
			}
		}
		if (count == array->len) {
			g_warning("Unable to find entry in exported menus");
			count = 0;
		}
	/* If we've got a list look there */
	} else if (list != NULL) {
		GList * head = list;
		while (head != NULL && head->data != entry) {
			count++;
			head = g_list_next(head);
		}
		if (head == NULL) {
			g_warning("Unable to find entry in exported menus");
			count = 0;
		}
	} else {
		g_warning("Looking for menus and we're not exporting any?");
	}

	return count;
}

/* Responds to a menuitem being activated on the panel. */
static void
entry_activate (IndicatorObject * io, IndicatorObjectEntry * entry, guint timestamp)
{
	return entry_activate_window(io, entry, 0, timestamp);
}

/* Responds to a menuitem being activated on the panel. */
static void
entry_activate_window (IndicatorObject * io, IndicatorObjectEntry * entry, guint windowid, guint timestamp)
{
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(io);

	/* We need to force a focus change in this case as we probably
	   just haven't gotten the signal from BAMF yet */
	if (windowid != 0) {
		GList * windows = bamf_matcher_get_windows(iapp->matcher);
		GList * window;
		BamfView * newwindow = NULL;

		for (window = windows; window != NULL; window = g_list_next(window)) {
			if (!BAMF_IS_WINDOW(window->data)) {
				continue;
			}

			BamfWindow * testwindow = BAMF_WINDOW(window->data);

			if (windowid == bamf_window_get_xid(testwindow)) {
				newwindow = BAMF_VIEW(testwindow);
				break;
			}
		}
		g_list_free(windows);

		if (newwindow != NULL) {
			active_window_changed(iapp->matcher, BAMF_VIEW(iapp->active_window), newwindow, iapp);
		}
	}

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

	/* Close any existing open menu by showing a null entry */
	window_show_menu(iapp->default_app, NULL, gtk_get_current_event_time(), iapp);

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

	guint32 xid = bamf_window_get_xid(iapp->active_window);
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

/* Find the label in a GTK MenuItem */
GtkLabel *
mi_find_label (GtkWidget * mi)
{
	if (GTK_IS_LABEL(mi)) {
		return GTK_LABEL(mi);
	}

	GtkLabel * retval = NULL;

	if (GTK_IS_CONTAINER(mi)) {
		GList * children = gtk_container_get_children(GTK_CONTAINER(mi));
		GList * child = children;

		while (child != NULL && retval == NULL) {
			if (GTK_IS_WIDGET(child->data)) {
				retval = mi_find_label(GTK_WIDGET(child->data));
			}
			child = g_list_next(child);
		}

		g_list_free(children);
	}

	return retval;
}

/* Find the icon in a GTK MenuItem */
GtkImage *
mi_find_icon (GtkWidget * mi)
{
	if (GTK_IS_IMAGE(mi)) {
		return GTK_IMAGE(mi);
	}

	GtkImage * retval = NULL;

	if (GTK_IS_CONTAINER(mi)) {
		GList * children = gtk_container_get_children(GTK_CONTAINER(mi));
		GList * child = children;

		while (child != NULL && retval == NULL) {
			if (GTK_IS_WIDGET(child->data)) {
				retval = mi_find_icon(GTK_WIDGET(child->data));
			}
			child = g_list_next(child);
		}

		g_list_free(children);
	}

	return retval;
}

/* Check the menu and make sure we return it if it's a menu
   all proper like that */
GtkMenu *
mi_find_menu (GtkMenuItem * mi)
{
	GtkWidget * retval = gtk_menu_item_get_submenu(mi);
	if (GTK_IS_MENU(retval)) {
		return GTK_MENU(retval);
	} else {
		return NULL;
	}
}

/* Remove an entry from the list and signal appropriately */
static void
remove_entry (IndicatorAppmenu * iapp, GList * item)
{
	g_signal_emit_by_name(G_OBJECT(iapp), INDICATOR_OBJECT_SIGNAL_ENTRY_REMOVED, item->data);
	iapp->application_menus = g_list_delete_link(iapp->application_menus, item);
	return;
}

/* Add an entry to the array with either the items that are in the
   parameter list, or if they're undefined then we need to find
   them here. */
static gboolean
add_entry (IndicatorAppmenu * iapp, GtkMenuItem * mi, GList * before, GtkLabel * label, GtkImage * icon, GtkMenu * sub)
{
	if (!gtk_widget_get_visible(GTK_WIDGET(mi))) {
		return FALSE;
	}

	if (label == NULL && icon == NULL && sub == NULL) {
		label = mi_find_label(GTK_WIDGET(mi));
		icon = mi_find_icon(GTK_WIDGET(mi));
		sub = mi_find_menu(mi);
	}

	/* We need to build a new entry to handle the menuitem */
	IndicatorObjectEntry * entry = g_new0(IndicatorObjectEntry, 1);
	entry->parent_object = INDICATOR_OBJECT(iapp);
	entry->label = label;
	entry->image = icon;
	entry->menu = sub;
	entry->accessible_desc = NULL;
	entry->name_hint = "application-menus";

	if (before != NULL) {
		iapp->application_menus = g_list_insert_before(iapp->application_menus, before, entry);
	} else {
		iapp->application_menus = g_list_append(iapp->application_menus, entry);
	}

	g_signal_emit_by_name(G_OBJECT(iapp), INDICATOR_OBJECT_SIGNAL_ENTRY_ADDED, entry);

	return TRUE;
}

/* Take a GtkMenu and make it into our entries */
static void
sync_menu_to_app_entries (IndicatorAppmenu * iapp, GtkMenu * menu)
{
	GList * children = NULL;

	if (GTK_IS_CONTAINER(menu)) {
		children = gtk_container_get_children(GTK_CONTAINER(menu));
	}

	GList * child = children;
	GList * apphead = iapp->application_menus;

	/* Let's go through entries as we have them */
	while (apphead != NULL && child != NULL) {
		GtkMenuItem * mi = NULL;

		/* If it's not a menu item, move along the list */
		if (GTK_IS_MENU_ITEM(child->data)) {
			mi = GTK_MENU_ITEM(child->data);
		} else {
			child = g_list_next(child);
			continue;
		}

		if (!gtk_widget_get_visible(GTK_WIDGET(mi))) {
			/* If it's not visible we want to skip it */
			child = g_list_next(child);
			continue;
		}

		GtkLabel * label = NULL;
		GtkImage * icon = NULL;
		GtkMenu * sub = NULL;
		if (GTK_IS_MENU_ITEM(mi)) {
			label = mi_find_label(GTK_WIDGET(mi));
			icon = mi_find_icon(GTK_WIDGET(mi));
			sub = mi_find_menu(mi);
		}

		IndicatorObjectEntry * entry = (IndicatorObjectEntry *)apphead->data;

		/* Check to see if we're the same */
		if (entry->label == label && entry->image == icon && entry->menu == sub) {
			/* If we are move both pointers and continue */
			apphead = g_list_next(apphead);
			child = g_list_next(child);
			continue;
		}

		add_entry(iapp, mi, apphead, label, icon, sub);
		child = g_list_next(child);
	}

	GList * prev = NULL;
	if (apphead != NULL) {
		prev = g_list_previous(apphead);
	}

	while (apphead != NULL) {
		remove_entry(iapp, apphead);

		if (prev != NULL) {
			apphead = g_list_next(prev);
		} else {
			apphead = iapp->application_menus;
		}
	}

	while (child != NULL) {
		GtkMenuItem * mi = NULL;

		/* If it's not a menu item, move along the list */
		if (GTK_IS_MENU_ITEM(child->data)) {
			mi = GTK_MENU_ITEM(child->data);
		} else {
			child = g_list_next(child);
			continue;
		}

		add_entry(iapp, mi, NULL, NULL, NULL, NULL);

		child = g_list_next(child);
	}

	g_list_free(children);

	return;
}

/* If we're displaying a list of entries we need to remove those before
   we could switch to a different set of entries */
static void
disconnect_current_signals (IndicatorAppmenu * iapp)
{
	/* Disconnect signals */
	if (iapp->sig_status_changed != 0) {
		g_signal_handler_disconnect(G_OBJECT(iapp->default_app), iapp->sig_status_changed);
		iapp->sig_status_changed = 0;
	}
	if (iapp->sig_show_menu != 0) {
		g_signal_handler_disconnect(G_OBJECT(iapp->default_app), iapp->sig_show_menu);
		iapp->sig_show_menu = 0;
	}
	if (iapp->sig_menu_changed != 0) {
		g_signal_handler_disconnect(G_OBJECT(iapp->default_app), iapp->sig_menu_changed);
		iapp->sig_menu_changed = 0;
	}

	return;
}

/* Put back some entries to ensure we've got something to click on!  This might
   be the stubs in some cases */
static void
reconnect_signals (IndicatorAppmenu * iapp)
{
	/* If we're putting up a new window, let's do that now. */
	if (iapp->default_app != NULL) {
		/* Connect signals */
		iapp->sig_status_changed = g_signal_connect(G_OBJECT(iapp->default_app),
		                                           WINDOW_MENUS_SIGNAL_STATUS_CHANGED,
		                                           G_CALLBACK(window_status_changed),
		                                           iapp);
		iapp->sig_show_menu     = g_signal_connect(G_OBJECT(iapp->default_app),
		                                           WINDOW_MENUS_SIGNAL_SHOW_MENU,
		                                           G_CALLBACK(window_show_menu),
		                                           iapp);
		iapp->sig_menu_changed  = g_signal_connect(G_OBJECT(iapp->default_app),
		                                           WINDOW_MENUS_SIGNAL_MENU_CHANGED,
		                                           G_CALLBACK(window_menu_changed),
		                                           iapp);
	}

		/* Set up initial state for new entries if needed */
	if (iapp->default_app != NULL &&
            window_menus_get_status (iapp->default_app) != DBUSMENU_STATUS_NORMAL) {
		window_status_changed (iapp->default_app,
		                       window_menus_get_status (iapp->default_app),
		                       iapp);
	}

	return;
}

/* Small little function to go through the logic of what
   menus we should be using. */
GtkMenu *
get_current_menu (IndicatorAppmenu * iapp)
{
	GtkMenu * menus = NULL;

	if (iapp->default_app != NULL) {
		menus = window_menus_get_menu(iapp->default_app);
	}

	if (menus == NULL && iapp->active_window == NULL) {
		menus = window_menus_get_menu(iapp->desktop_menu);
	}

	return menus;
}

/* Switch applications, remove all the entires for the previous
   one and add them for the new application */
static void
switch_default_app (IndicatorAppmenu * iapp, WindowMenus * newdef, BamfWindow * active_window)
{
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

	disconnect_current_signals(iapp);

	/* Default App is NULL, let's see if it needs replacement */
	iapp->default_app = NULL;

	/* Update the active window pointer -- may be NULL */
	switch_active_window(iapp, active_window);

	/* If we're putting up a new window, let's do that now. */
	if (newdef != NULL) {
		/* Switch */
		iapp->default_app = newdef;
	}

	GtkMenu * menus = get_current_menu(iapp);

	if (iapp->menu_mode == MENU_MODE_SEVERAL) {
		sync_menu_to_app_entries(iapp, menus);	
		reconnect_signals(iapp);
	} else {
		iapp->single_menu.menu = menus;

		gtk_widget_set_sensitive(GTK_WIDGET(iapp->single_menu.image), menus != NULL);

		if (iapp->default_app != NULL) {
			iapp->sig_show_menu     = g_signal_connect(G_OBJECT(iapp->default_app),
			                                           WINDOW_MENUS_SIGNAL_SHOW_MENU,
			                                           G_CALLBACK(single_show_menu),
			                                           iapp);
		}
	}

	return;
}

/* Recieve the signal that the window being shown
   has now changed. */
static void
active_window_changed (BamfMatcher * matcher, BamfView * oldview, BamfView * newview, gpointer user_data)
{
	BamfWindow * window = NULL;

	if (newview != NULL && BAMF_IS_WINDOW(newview)) {
		if (BAMF_IS_WINDOW(newview)) {
			window = BAMF_WINDOW(newview);
		} else {
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
	if (BAMF_IS_WINDOW(newview)) {
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

	guint32 xid = window_menus_get_xid(wm);
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

	/* Shouldn't do anything, but let's be sure */
	g_hash_table_remove(iapp->destruction_timers, GUINT_TO_POINTER(windowid));

	if (g_hash_table_lookup(iapp->apps, GUINT_TO_POINTER(windowid)) == NULL && windowid != 0) {
		WindowMenus * wm = window_menus_new(windowid, sender, objectpath);
		g_return_val_if_fail(wm != NULL, FALSE);

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
			g_warning("Already have a menu for window ID %d with path %s from %s, unregistering that one", windowid, objectpath, sender);
			unregister_window(iapp, windowid);

			/* NOTE: So we're doing a lookup here.  That seems pretty useless
			   now doesn't it.  It's for a good reason.  We're going recursive
			   with a pretty complex set of functions we want to ensure that
			   we're not going to end up infinitely recursive otherwise things
			   could go really bad. */
			if (g_hash_table_lookup(iapp->apps, GUINT_TO_POINTER(windowid)) == NULL) {
				return register_window(iapp, windowid, objectpath, sender);
			}

			g_warning("Unable to unregister window!");
		}
	}

	return g_variant_new("()");
}

/* Kindly remove an entry from our DB */
static GVariant *
unregister_window (IndicatorAppmenu * iapp, guint windowid)
{
	g_debug("Unregistering: %d", windowid);
	g_return_val_if_fail(IS_INDICATOR_APPMENU(iapp), NULL);
	g_return_val_if_fail(iapp->matcher != NULL, NULL);

	/* Make sure we don't destroy it later */
	g_hash_table_remove(iapp->destruction_timers, GUINT_TO_POINTER(windowid));

	/* If it's a desktop window remove it from that table as well */
	g_hash_table_remove(iapp->desktop_windows, GUINT_TO_POINTER(windowid));

	/* Now let's see if we've got a WM object for it then
	   we need to mark it as destroyed and unreference to
	   actually destroy it. */
	gpointer wm = g_hash_table_lookup(iapp->apps, GUINT_TO_POINTER(windowid));
	if (wm != NULL) {
		GObject * wmo = G_OBJECT(wm);

		/* Using destroyed so that if the menus are shown
		   they'll be switch and the current window gets
		   updated as well. */
		menus_destroyed(wmo, iapp);
		g_object_unref(wmo);
	}

	return NULL;
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

	GVariantBuilder array;
	g_variant_builder_init (&array, G_VARIANT_TYPE_ARRAY);
	GList * appkeys = NULL;
	for (appkeys = g_hash_table_get_keys(iapp->apps); appkeys != NULL; appkeys = g_list_next(appkeys)) {
		gpointer hash_val = g_hash_table_lookup(iapp->apps, appkeys->data);

		if (hash_val == NULL) { continue; }

		GVariantBuilder tuple;
		g_variant_builder_init(&tuple, G_VARIANT_TYPE_TUPLE);
		g_variant_builder_add_value(&tuple, g_variant_new_uint32(window_menus_get_xid(WINDOW_MENUS(hash_val))));
		g_variant_builder_add_value(&tuple, g_variant_new_string(window_menus_get_address(WINDOW_MENUS(hash_val))));
		g_variant_builder_add_value(&tuple, g_variant_new_object_path(window_menus_get_path(WINDOW_MENUS(hash_val))));

		g_variant_builder_add_value(&array, g_variant_builder_end(&tuple));
	}

	GVariant * varray = g_variant_builder_end(&array);
	return g_variant_new_tuple(&varray, 1);
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
		                                           "com.canonical.AppMenu.Error",
		                                           error->message);
		g_error_free(error);
	} else {
		g_dbus_method_invocation_return_value(invocation, retval);
	}
	return;
}

/* Pass up the status changed event */
static void
window_status_changed (WindowMenus * mw, DbusmenuStatus status, IndicatorAppmenu * iapp)
{
	gboolean show_now = (status == DBUSMENU_STATUS_NOTICE);
	GList * entry_head, * entries;

	entry_head = indicator_object_get_entries(INDICATOR_OBJECT(iapp));

	for (entries = entry_head; entries != NULL; entries = g_list_next(entries)) {
		IndicatorObjectEntry * entry = (IndicatorObjectEntry *)entries->data;
		g_signal_emit(G_OBJECT(iapp), INDICATOR_OBJECT_SIGNAL_SHOW_NOW_CHANGED_ID, 0, entry, show_now);
	}

	return;
}

/* when we show a menu in the single menu case we just want to
   ensure that we signal the single menu is being shown so that
   the panel can place it correctly. */
static void
single_show_menu (WindowMenus * mw, GtkMenuItem * item, guint timestamp, gpointer user_data)
{
	IndicatorAppmenu * iapp = INDICATOR_APPMENU(user_data);
	g_signal_emit_by_name(G_OBJECT(user_data), INDICATOR_OBJECT_SIGNAL_MENU_SHOW, &(iapp->single_menu), timestamp);
	return;
}

/* Pass up the show menu event */
static void
window_show_menu (WindowMenus * mw, GtkMenuItem * item, guint timestamp, gpointer user_data)
{
	if (item == NULL) {
		g_signal_emit_by_name(G_OBJECT(user_data), INDICATOR_OBJECT_SIGNAL_MENU_SHOW, NULL, timestamp);
		return;
	}

	GtkMenu * sub = GTK_MENU(gtk_menu_item_get_submenu(item));
	if (sub == NULL) {
		return;
	}

	IndicatorAppmenu * iapp = INDICATOR_APPMENU(user_data);
	GList * lentry;
	for (lentry = iapp->application_menus; lentry != NULL; lentry = g_list_next(lentry)) {
		IndicatorObjectEntry * entry = (IndicatorObjectEntry *)lentry->data;

		if (entry->menu == sub) {
			g_signal_emit_by_name(G_OBJECT(user_data), INDICATOR_OBJECT_SIGNAL_MENU_SHOW, entry, timestamp);
			break;
		}
	}

	return;
}

/* Pass up the show menu event */
static void
window_menu_changed (WindowMenus * mw, gpointer user_data)
{
	sync_menu_to_app_entries(INDICATOR_APPMENU(user_data), window_menus_get_menu(mw));
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

