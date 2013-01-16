/*
Make sure the app info is truly bad and we don't crash or something.

Copyright 2011 Canonical Ltd.

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

#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glib-object.h>
#include "load-app-info.h"

static void
build_db (sqlite3 * db)
{
	/* Create the table */
	int exec_status = SQLITE_OK;
	gchar * failstring = NULL;
	exec_status = sqlite3_exec(db,
	                           "create table usage (application text, entry text, timestamp datetime);",
	                           NULL, NULL, &failstring);
	if (exec_status != SQLITE_OK) {
		g_warning("Unable to create table: %s", failstring);
	}

	/* Import data from the system */

	return;
}

int
main (int argv, char * argc[])
{
	gboolean passed = TRUE;

	if (argv != 2) {
		g_printerr("Usage: %s <app-info file path>\n", argc[0]);
		return 1;
	}

	g_type_init();

	gchar * filename = NULL;
	gint tmpfile = g_file_open_tmp("hud-test-bad-app-info-temp-db-XXXXXX", &filename, NULL);

	if (tmpfile < 0) {
		passed = FALSE;
	} else {

		close(tmpfile);
		/* NOTE: there is a small security bug here in that we're closing the
		   file and reopening it, so the temp isn't really gauranteed to be
		   safe.  But, I don't think we're really worried about security in this
		   utility. */

		sqlite3 * db = NULL;
		int open_status = sqlite3_open(filename, &db); 

		if (open_status != SQLITE_OK) {
			g_warning("Error opening usage DB: %s", filename);
			passed = FALSE;
		} else {

			/* Create the table in the DB */
			build_db(db);

			passed = !load_app_info(argc[1], db);

			sqlite3_close(db);
		}
	}

	if (filename != NULL) {
		if (g_getenv("HUD_VERIFY_NO_UNLINK") == NULL) {
			g_unlink(filename);
		} else {
			g_print("Temp db '%s' not deleted\n", filename);
		}
		g_free(filename);
	}

	if (passed) {
		return 0;
	} else {
		return 1;
	}
}