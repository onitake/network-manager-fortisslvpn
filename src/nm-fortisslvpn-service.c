/* nm-fortisslvpn-service - SSLVPN integration with NetworkManager
 *
 * Lubomir Rintel <lkundrak@v3.sk>
 * Dan Williams <dcbw@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * (C) Copyright 2008 - 2014 Red Hat, Inc.
 * (C) Copyright 2015 Lubomir Rintel
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ctype.h>
#include <locale.h>
#include <errno.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>

#include "nm-fortisslvpn-service.h"
#include "nm-ppp-status.h"
#include "nm-fortisslvpn-pppd-service-dbus.h"

#if !defined(DIST_VERSION)
# define DIST_VERSION VERSION
#endif

static gboolean debug = FALSE;

/********************************************************/
/* The VPN plugin service                               */
/********************************************************/

static void nm_fortisslvpn_plugin_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (NMFortisslvpnPlugin, nm_fortisslvpn_plugin, NM_TYPE_VPN_SERVICE_PLUGIN,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, nm_fortisslvpn_plugin_initable_iface_init));

typedef struct {
	GPid pid;
	guint32 ppp_timeout_handler;
	NMConnection *connection;
	char *config_file;
	NMDBusFortisslvpnPpp *dbus_skeleton;
	/* These are the stdin and stdout of the daemon process.
	 * Use them to capture error messages and send input,
	 * for example for 2-factor authentication.
	 */
	GIOChannel *in;
	GIOChannel *out;
	/* The I/O watch handles for in and out */
	guint in_watch;
	guint out_watch;
	/* TRUE if we're waiting for a 2-factor secret */
	gboolean wait_2factor;
} NMFortisslvpnPluginPrivate;

#define NM_FORTISSLVPN_PLUGIN_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_FORTISSLVPN_PLUGIN, NMFortisslvpnPluginPrivate))

#define NM_FORTISSLVPN_PPPD_PLUGIN PLUGINDIR "/nm-fortisslvpn-pppd-plugin.so"
#define NM_FORTISSLVPN_WAIT_PPPD 10000 /* 10 seconds */
#define FORTISSLVPN_SERVICE_SECRET_TRIES "fortisslvpn-service-secret-tries"

#define NM_OPENFORTIVPN_MSGPFX_2FACTOR "2factor"
#define NM_OPENFORTIVPN_MSGSFX_2FACTOR " authentication token:"
#define NM_OPENFORTIVPN_MSGPFX_ERROR "ERROR: "
#define NM_OPENFORTIVPN_MSGPFX_WARN "WARN:  "
#define NM_OPENFORTIVPN_MSGPFX_INFO "INFO:  "
#define NM_OPENFORTIVPN_PREFIX_LENGTH sizeof(NM_OPENFORTIVPN_MSGPFX_2FACTOR)

typedef struct {
	const char *name;
	GType type;
	gboolean required;
} ValidProperty;

static ValidProperty valid_properties[] = {
	{ NM_FORTISSLVPN_KEY_GATEWAY,           G_TYPE_STRING, TRUE },
	{ NM_FORTISSLVPN_KEY_USER,              G_TYPE_STRING, TRUE },
	{ NM_FORTISSLVPN_KEY_CA,                G_TYPE_STRING, FALSE },
	{ NM_FORTISSLVPN_KEY_TRUSTED_CERT,      G_TYPE_STRING, FALSE },
	{ NM_FORTISSLVPN_KEY_CERT,              G_TYPE_STRING, FALSE },
	{ NM_FORTISSLVPN_KEY_KEY,               G_TYPE_STRING, FALSE },
	{ NM_FORTISSLVPN_KEY_PASSWORD"-flags",  G_TYPE_UINT, FALSE },
	{ NULL,                                 G_TYPE_NONE, FALSE }
};

static ValidProperty valid_secrets[] = {
	{ NM_FORTISSLVPN_KEY_PASSWORD,          G_TYPE_STRING, TRUE },
	{ NM_FORTISSLVPN_KEY_2FACTOR,           G_TYPE_STRING, FALSE },
	{ NULL,                                 G_TYPE_NONE,   FALSE }
};

static gboolean
validate_gateway (const char *gateway)
{
	if (!gateway || !strlen (gateway) || !isalnum (*gateway))
		return FALSE;

	return TRUE;
}

/* This is a bit half-assed. We should check that the user doesn't
 * abuse this to access files he ordinarily shouldn't, but we an't do
 * any better than this for we don't have any information about the
 * identity of the user that activates the connection.
 * We should probably get the certificate inline or something. */
static gboolean
validate_ca (const char *ca)
{
	struct stat ca_stat;

	/* Tolerate only absolute paths */
	if (!ca || !strlen (ca) || *ca != '/')
		return FALSE;

	if (stat (ca, &ca_stat) == -1)
		return FALSE;

	/* Allow only ordinary files */
	if (!(ca_stat.st_mode & S_IFREG))
		return FALSE;

	/* Allow only world-readable files */
	if ((ca_stat.st_mode & 0444) != 0444)
		return FALSE;

	return TRUE;
}

typedef struct ValidateInfo {
	ValidProperty *table;
	GError **error;
	gboolean have_items;
} ValidateInfo;

static void
validate_one_property (const char *key, const char *value, gpointer user_data)
{
	ValidateInfo *info = (ValidateInfo *) user_data;
	int i;

	if (*(info->error))
		return;

	info->have_items = TRUE;

	/* 'name' is the setting name; always allowed but unused */
	if (!strcmp (key, NM_SETTING_NAME))
		return;

	for (i = 0; info->table[i].name; i++) {
		ValidProperty prop = info->table[i];
		long int tmp;

		if (strcmp (prop.name, key))
			continue;

		switch (prop.type) {
		case G_TYPE_STRING:
			if (   !strcmp (prop.name, NM_FORTISSLVPN_KEY_GATEWAY)
			    && !validate_gateway (value)) {
				g_set_error (info->error,
				             NM_VPN_PLUGIN_ERROR,
				             NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
				             _("invalid gateway '%s'"),
				             value);
				return;
			} else if (   !strcmp (prop.name, NM_FORTISSLVPN_KEY_CA)
			           && !validate_ca (value)) {
				g_set_error (info->error,
				             NM_VPN_PLUGIN_ERROR,
				             NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
				             _("invalid certificate authority '%s'"),
				             value);
				return;
			}
			return; /* valid */
		case G_TYPE_UINT:
			errno = 0;
			tmp = strtol (value, NULL, 10);
			if (errno == 0)
				return; /* valid */

			g_set_error (info->error,
			             NM_VPN_PLUGIN_ERROR,
			             NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
			             _("invalid integer property '%s'"),
			             key);
			break;
		case G_TYPE_BOOLEAN:
			if (!strcmp (value, "yes") || !strcmp (value, "no"))
				return; /* valid */

			g_set_error (info->error,
			             NM_VPN_PLUGIN_ERROR,
			             NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
			             _("invalid boolean property '%s' (not yes or no)"),
			             key);
			break;
		default:
			g_set_error (info->error,
			             NM_VPN_PLUGIN_ERROR,
			             NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
			             _("unhandled property '%s' type %s"),
			             key, g_type_name (prop.type));
			break;
		}
	}

	/* Did not find the property from valid_properties or the type did not match */
	if (!info->table[i].name) {
		g_set_error (info->error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
		             _("property '%s' invalid or not supported"),
		             key);
	}
}

static gboolean
validate_properties (NMSettingVpn *s_vpn, GError **error)
{
	ValidateInfo info = { &valid_properties[0], error, FALSE };
	int i;

	nm_setting_vpn_foreach_data_item (s_vpn, validate_one_property, &info);
	if (!info.have_items) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
		             "%s",
		             _("No VPN configuration options."));
		return FALSE;
	}

	if (*error)
		return FALSE;

	/* Ensure required properties exist */
	for (i = 0; valid_properties[i].name; i++) {
		ValidProperty prop = valid_properties[i];
		const char *value;

		if (!prop.required)
			continue;

		value = nm_setting_vpn_get_data_item (s_vpn, prop.name);
		if (!value || !strlen (value)) {
			g_set_error (error,
			             NM_VPN_PLUGIN_ERROR,
			             NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
			             _("Missing required option '%s'."),
			             prop.name);
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
validate_secrets (NMSettingVpn *s_vpn, GError **error)
{
	ValidateInfo info = { &valid_secrets[0], error, FALSE };

	nm_setting_vpn_foreach_secret (s_vpn, validate_one_property, &info);
	if (!info.have_items) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
		             "%s",
		             _("No VPN secrets!"));
		return FALSE;
	}

	return *error ? FALSE : TRUE;
}

static gboolean
ensure_killed (gpointer data)
{
	int pid = GPOINTER_TO_INT (data);

	if (kill (pid, 0) == 0)
		kill (pid, SIGKILL);

	return FALSE;
}

static void
destroy_child_io (NMFortisslvpnPlugin *plugin)
{
	NMFortisslvpnPluginPrivate *priv = NM_FORTISSLVPN_PLUGIN_GET_PRIVATE (plugin);
	
	if (priv->in) {
		g_source_remove(priv->in_watch);
		g_clear_object(&priv->in);
	}
	if (priv->out) {
		g_source_remove(priv->out_watch);
		g_clear_object(&priv->in);
	}
}

static void
cleanup_plugin (NMFortisslvpnPlugin *plugin)
{
	NMFortisslvpnPluginPrivate *priv = NM_FORTISSLVPN_PLUGIN_GET_PRIVATE (plugin);

	destroy_child_io(plugin);
	
	if (priv->pid) {
		if (kill (priv->pid, SIGTERM) == 0)
			g_timeout_add (2000, ensure_killed, GINT_TO_POINTER (priv->pid));
		else
			kill (priv->pid, SIGKILL);

		g_message ("Terminated ppp daemon with PID %d.", priv->pid);
		priv->pid = 0;
	}

	g_clear_object (&priv->connection);
	if (priv->config_file) {
		g_unlink (priv->config_file);
		g_clear_pointer (&priv->config_file, g_free);
	}
}

static void
openfortivpn_watch_cb (GPid pid, gint status, gpointer user_data)
{
	NMFortisslvpnPlugin *plugin = NM_FORTISSLVPN_PLUGIN (user_data);
	NMFortisslvpnPluginPrivate *priv = NM_FORTISSLVPN_PLUGIN_GET_PRIVATE (plugin);
	guint error = 0;

	if (WIFEXITED (status))
		error = WEXITSTATUS (status);
	else if (WIFSTOPPED (status))
		g_warning ("openfortivpn stopped unexpectedly with signal %d", WSTOPSIG (status));
	else if (WIFSIGNALED (status))
		g_warning ("openfortivpn died with signal %d", WTERMSIG (status));
	else
		g_warning ("openfortivpn died from an unknown cause");

	
	destroy_child_io(plugin);
	
	/* Reap child if needed. */
	waitpid (priv->pid, NULL, WNOHANG);
	priv->pid = 0;

	/* TODO: Better error indication from openfortivpn. */
	if (error)
		nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (plugin),
		                               NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
	else
		nm_vpn_service_plugin_disconnect (NM_VPN_SERVICE_PLUGIN (plugin), NULL);
}

static const char *
nm_find_openfortivpn (void)
{
	static const char *openfortivpn_binary_paths[] =
		{
			"/bin/openfortivpn",
			"/usr/bin/openfortivpn",
			"/usr/local/bin/openfortivpn",
			NULL
		};

	const char **openfortivpn_binary = openfortivpn_binary_paths;

	while (*openfortivpn_binary != NULL) {
		if (g_file_test (*openfortivpn_binary, G_FILE_TEST_EXISTS))
			break;
		openfortivpn_binary++;
	}

	return *openfortivpn_binary;
}

static gboolean
pppd_timed_out (gpointer user_data)
{
	NMFortisslvpnPlugin *plugin = NM_FORTISSLVPN_PLUGIN (user_data);

	g_warning ("Looks like pppd didn't initialize our dbus module");
	nm_vpn_service_plugin_failure (NM_VPN_SERVICE_PLUGIN (plugin), NM_VPN_CONNECTION_STATE_REASON_SERVICE_START_TIMEOUT);

	return FALSE;
}

static gboolean
openfortivpn_stdin_cb (GIOChannel *source, GIOCondition condition, gpointer user_data)
{
	NMFortisslvpnPlugin *plugin = NM_FORTISSLVPN_PLUGIN (user_data);
	NMFortisslvpnPluginPrivate *priv = NM_FORTISSLVPN_PLUGIN_GET_PRIVATE (plugin);
	
	if (condition == G_IO_HUP) {
		g_clear_object(&priv->in);
		return FALSE;
	}

	return TRUE;
}

static gboolean
openfortivpn_stdout_cb (GIOChannel *source, GIOCondition condition, gpointer user_data)
{
	NMFortisslvpnPlugin *plugin = NM_FORTISSLVPN_PLUGIN (user_data);
	NMFortisslvpnPluginPrivate *priv = NM_FORTISSLVPN_PLUGIN_GET_PRIVATE (plugin);
	gchar token[NM_OPENFORTIVPN_PREFIX_LENGTH + sizeof(NM_OPENFORTIVPN_MSGSFX_2FACTOR)];
	gsize token_length = 0;
	gchar *line = NULL;
	gsize line_length = 0;
	GVariantBuilder *builder;
	GVariant *msg;
	GVariant *keys;
	
	if (condition == G_IO_HUP) {
		g_clear_object(&priv->out);
		return FALSE;
	}
	
	g_io_channel_read_chars(source, token, NM_OPENFORTIVPN_PREFIX_LENGTH, &token_length, NULL);
	
	/* TODO Pass the messages to the UI instead of the log */
	
    /* Evaluate the output */
	if (token_length == NM_OPENFORTIVPN_PREFIX_LENGTH && g_strstr_len(token, token_length, "2factor") == token) {
		/* Line starts with the 2-factor auth prompt */
		g_io_channel_read_chars(source, token, sizeof(token), &token_length, NULL);
		
		/* Waiting for a reply... */
		priv->wait_2factor = TRUE;
		
		/* Send a message to the VPN plugin that we need another auth token */
		msg = g_variant_new_string (_("Please enter the 2-factor authentication token:"));
		builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
		g_variant_builder_add (builder, "s", NM_FORTISSLVPN_KEY_2FACTOR);
		keys = g_variant_new ("as");
		g_variant_builder_unref (builder);
		g_signal_emit_by_name (plugin, "SecretsRequired", msg, keys);
		g_variant_unref (msg);
		g_variant_unref (keys);
		
	} else if (token_length == NM_OPENFORTIVPN_PREFIX_LENGTH && g_strstr_len (token, token_length, "ERROR: ") == token) {
		/* Line is an error message */
		g_io_channel_read_to_end(source, &line, &line_length, NULL);
		g_io_channel_read_line(source, &line, &line_length, NULL, NULL);
		g_error("openfortivpn: %.*s", (int) line_length, line);
		g_free(line);
	} else if (token_length == NM_OPENFORTIVPN_PREFIX_LENGTH && g_strstr_len (token, token_length, "WARN:  ") == token) {
		/* Line is a warning */
		g_io_channel_read_line(source, &line, &line_length, NULL, NULL);
		g_warning("openfortivpn: %.*s", (int) line_length, line);
		g_free(line);
	} else if (token_length == NM_OPENFORTIVPN_PREFIX_LENGTH && g_strstr_len (token, token_length, "INFO:  ") == token) {
		/* Line is informational */
		g_io_channel_read_line(source, &line, &line_length, NULL, NULL);
		g_info("openfortivpn: %.*s", (int) line_length, line);
		g_free(line);
	} else {
		/* Line is something else */
		g_io_channel_read_line(source, &line, &line_length, NULL, NULL);
		g_debug("openfortivpn: %.*s%.*s", (int) token_length, token, (int) line_length, line);
		g_free(line);
	}
	
	return TRUE;
}

static gboolean
run_openfortivpn (NMFortisslvpnPlugin *plugin, NMSettingVpn *s_vpn, GError **error)
{
	NMFortisslvpnPluginPrivate *priv = NM_FORTISSLVPN_PLUGIN_GET_PRIVATE (plugin);
	GPid pid;
	const char *openfortivpn;
	GPtrArray *argv;
	const char *value;
	gint in = -1, out = -1, err = -1;

	openfortivpn = nm_find_openfortivpn ();
	if (!openfortivpn) {
		g_set_error (error,
		             NM_VPN_PLUGIN_ERROR,
		             NM_VPN_PLUGIN_ERROR_LAUNCH_FAILED,
		             "%s",
		             _("Could not find the openfortivpn binary."));
		return FALSE;
	}

	argv = g_ptr_array_new_with_free_func (g_free);
	g_ptr_array_add (argv, (gpointer) g_strdup (openfortivpn));

	g_ptr_array_add (argv, (gpointer) g_strdup ("-c"));
	g_ptr_array_add (argv, (gpointer) g_strdup (priv->config_file));

	g_ptr_array_add (argv, (gpointer) g_strdup ("--no-routes"));
	g_ptr_array_add (argv, (gpointer) g_strdup ("--no-dns"));

	value = nm_setting_vpn_get_data_item (s_vpn, NM_FORTISSLVPN_KEY_GATEWAY);
	g_ptr_array_add (argv, (gpointer) g_strdup (value));

	if (debug)
		g_ptr_array_add (argv, (gpointer) g_strdup ("-vvv"));

	value = nm_setting_vpn_get_data_item (s_vpn, NM_FORTISSLVPN_KEY_CA);
	if (value) {
		g_ptr_array_add (argv, (gpointer) g_strdup ("--ca-file"));
		g_ptr_array_add (argv, (gpointer) g_strdup (value));
	}

	value = nm_setting_vpn_get_data_item (s_vpn, NM_FORTISSLVPN_KEY_CERT);
	if (value) {
		g_ptr_array_add (argv, (gpointer) g_strdup ("--user-cert"));
		g_ptr_array_add (argv, (gpointer) g_strdup (value));
	}

	value = nm_setting_vpn_get_data_item (s_vpn, NM_FORTISSLVPN_KEY_KEY);
	if (value) {
		g_ptr_array_add (argv, (gpointer) g_strdup ("--user-key"));
		g_ptr_array_add (argv, (gpointer) g_strdup (value));
	}

	value = nm_setting_vpn_get_data_item (s_vpn, NM_FORTISSLVPN_KEY_TRUSTED_CERT);
	if (value) {
		g_ptr_array_add (argv, (gpointer) g_strdup ("--trusted-cert"));
		g_ptr_array_add (argv, (gpointer) g_strdup (value));
	}

	g_ptr_array_add (argv, (gpointer) g_strdup ("--pppd-plugin"));
	g_ptr_array_add (argv, (gpointer) g_strdup (NM_FORTISSLVPN_PPPD_PLUGIN));

	g_ptr_array_add (argv, NULL);

	/* TODO should be run with the C locale, so error messages can be processed */
	if (!g_spawn_async_with_pipes (NULL, (char **) argv->pdata, NULL,
	                    G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, 
						&in, &out, &err,
						error)) {
		g_ptr_array_free (argv, TRUE);
		return FALSE;
	}
	g_ptr_array_free (argv, TRUE);

	g_message ("openfortivpn started with pid %d", pid);

#ifdef G_OS_WIN32
	priv->in = g_io_channel_win32_new_fd (in);
	priv->out = g_io_channel_win32_new_fd (out);
#else
	priv->in = g_io_channel_unix_new (in);
	priv->out = g_io_channel_unix_new (out);
#endif
	priv->in_watch = g_io_add_watch (priv->in, G_IO_HUP, (GIOFunc) openfortivpn_stdin_cb, plugin);
	priv->out_watch = g_io_add_watch (priv->out, G_IO_IN | G_IO_HUP, (GIOFunc) openfortivpn_stdout_cb, plugin);
	
	priv->pid = pid;
	g_child_watch_add (pid, openfortivpn_watch_cb, plugin);

	priv->ppp_timeout_handler = g_timeout_add (NM_FORTISSLVPN_WAIT_PPPD, pppd_timed_out, plugin);

	return TRUE;
}

static void
remove_timeout_handler (NMFortisslvpnPlugin *plugin)
{
	NMFortisslvpnPluginPrivate *priv = NM_FORTISSLVPN_PLUGIN_GET_PRIVATE (plugin);
	
	if (priv->ppp_timeout_handler) {
		g_source_remove (priv->ppp_timeout_handler);
		priv->ppp_timeout_handler = 0;
	}
}

static gboolean
handle_set_state (NMDBusFortisslvpnPpp *object,
                  GDBusMethodInvocation *invocation,
                  guint arg_state,
                  gpointer user_data)
{
	remove_timeout_handler (NM_FORTISSLVPN_PLUGIN (user_data));
	if (arg_state == NM_PPP_STATUS_DEAD || arg_state == NM_PPP_STATUS_DISCONNECT)
		nm_vpn_service_plugin_disconnect (NM_VPN_SERVICE_PLUGIN (user_data), NULL);

	nmdbus_fortisslvpn_ppp_complete_set_state (object, invocation);
	return TRUE;
}

static gboolean
handle_set_ip4_config (NMDBusFortisslvpnPpp *object,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_config,
                       gpointer user_data)
{
	g_message ("FORTISSLVPN service (IP Config Get) reply received.");

	remove_timeout_handler (NM_FORTISSLVPN_PLUGIN (user_data));
	nm_vpn_service_plugin_set_ip4_config (NM_VPN_SERVICE_PLUGIN (user_data), arg_config);

	nmdbus_fortisslvpn_ppp_complete_set_ip4_config (object, invocation);
	return TRUE;
}

static gboolean
get_credentials (NMSettingVpn *s_vpn,
                 const char **username,
                 const char **password,
                 GError **error)
{
	/* Username; try SSLVPN specific username first, then generic username */
	*username = nm_setting_vpn_get_data_item (s_vpn, NM_FORTISSLVPN_KEY_USER);
	if (!*username || !strlen (*username)) {
		*username = nm_setting_vpn_get_user_name (s_vpn);
		if (!*username || !strlen (*username)) {
			g_set_error_literal (error,
			                     NM_VPN_PLUGIN_ERROR,
			                     NM_VPN_PLUGIN_ERROR_INVALID_CONNECTION,
			                     _("Missing VPN username."));
			return FALSE;
		}
	}

	*password = nm_setting_vpn_get_secret (s_vpn, NM_FORTISSLVPN_KEY_PASSWORD);
	if (!*password || !strlen (*password)) {
		g_set_error_literal (error,
		                     NM_VPN_PLUGIN_ERROR,
		                     NM_VPN_PLUGIN_ERROR_INVALID_CONNECTION,
		                     _("Missing or invalid VPN password."));
		return FALSE;
	}

	return TRUE;
}

static gboolean
real_connect (NMVpnServicePlugin *plugin, NMConnection *connection, GError **error)
{
	NMFortisslvpnPluginPrivate *priv = NM_FORTISSLVPN_PLUGIN_GET_PRIVATE (plugin);
	NMSettingVpn *s_vpn;
	mode_t old_umask;
	gchar *config;
	const char *username, *password;

	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	s_vpn = NM_SETTING_VPN (nm_connection_get_setting (connection, NM_TYPE_SETTING_VPN));
	g_assert (s_vpn);

	if (!validate_properties (s_vpn, error))
		return FALSE;

	if (!validate_secrets (s_vpn, error))
		return FALSE;

	if (!get_credentials (s_vpn, &username, &password, error))
		return FALSE;

	g_clear_object (&priv->connection);
	priv->connection = g_object_ref (connection);

	if (debug)
		nm_connection_dump (connection);

	/* Create the configuration file so that we don't expose
	 * secrets on the command line */
	priv->config_file = g_strdup_printf (NM_FORTISSLVPN_STATEDIR "/%s.config",
	                                     nm_connection_get_uuid (connection));
	config = g_strdup_printf ("username = %s\npassword = %s\n",
	                          username, password);
	old_umask = umask (0077);
	if (!g_file_set_contents (priv->config_file, config, -1, error)) {
		g_clear_pointer (&priv->config_file, g_free);
		umask (old_umask);
		g_free (config);
		return FALSE;
	}
	umask (old_umask);
	g_free (config);

	/* Run the acutal openfortivpn process */
	return run_openfortivpn (NM_FORTISSLVPN_PLUGIN (plugin), s_vpn, error);
}

static gboolean
real_need_secrets (NMVpnServicePlugin *plugin,
                   NMConnection *connection,
                   const char **setting_name,
                   GError **error)
{
	NMSetting *s_vpn;
	NMSettingSecretFlags flags = NM_SETTING_SECRET_FLAG_NONE;

	g_return_val_if_fail (NM_IS_VPN_SERVICE_PLUGIN (plugin), FALSE);
	g_return_val_if_fail (NM_IS_CONNECTION (connection), FALSE);

	s_vpn = nm_connection_get_setting (connection, NM_TYPE_SETTING_VPN);

	nm_setting_get_secret_flags (NM_SETTING (s_vpn), NM_FORTISSLVPN_KEY_PASSWORD, &flags, NULL);

	/* Don't need the password if it's not required */
	if (flags & NM_SETTING_SECRET_FLAG_NOT_REQUIRED)
		return FALSE;

	/* Don't need the password if we already have one */
	if (nm_setting_vpn_get_secret (NM_SETTING_VPN (s_vpn), NM_FORTISSLVPN_KEY_PASSWORD))
		return FALSE;

	/* Otherwise we need a password */
	*setting_name = NM_SETTING_VPN_SETTING_NAME;
	return TRUE;
}

static gboolean
real_new_secrets(NMVpnServicePlugin *plugin,
                 NMConnection *connection,
                 GError **error)
{
	NMFortisslvpnPluginPrivate *priv = NM_FORTISSLVPN_PLUGIN_GET_PRIVATE(plugin);
	NMSetting *s_vpn;
	const char *twofactor;

	g_return_val_if_fail (NM_IS_VPN_SERVICE_PLUGIN (plugin), FALSE);
	g_return_val_if_fail (NM_IS_CONNECTION (connection), FALSE);
	g_return_val_if_fail (priv->in, FALSE);
	g_return_val_if_fail (priv->wait_2factor, FALSE);
	
	s_vpn = nm_connection_get_setting(connection, NM_TYPE_SETTING_VPN);

	/* Check if we received a 2-factor token */
	twofactor = nm_setting_vpn_get_secret (NM_SETTING_VPN (s_vpn), NM_FORTISSLVPN_KEY_2FACTOR);
	if (!twofactor)
		return FALSE;

	/* Yes, send it to the child */
	g_info("Got two-factor token: %s", twofactor);
	g_io_channel_write_chars (priv->in, twofactor, -1, NULL, NULL);
	
	/* And we're done waiting */
	priv->wait_2factor = FALSE;

	return TRUE;

}

static gboolean
real_disconnect (NMVpnServicePlugin *plugin, GError **err)
{
	cleanup_plugin (NM_FORTISSLVPN_PLUGIN (plugin));
	return TRUE;
}

static void
state_changed_cb (GObject *object, NMVpnServiceState state, gpointer user_data)
{
	NMFortisslvpnPluginPrivate *priv = NM_FORTISSLVPN_PLUGIN_GET_PRIVATE (object);

	switch (state) {
	case NM_VPN_SERVICE_STATE_STARTED:
		remove_timeout_handler (NM_FORTISSLVPN_PLUGIN (object));
		break;
	case NM_VPN_SERVICE_STATE_UNKNOWN:
	case NM_VPN_SERVICE_STATE_INIT:
	case NM_VPN_SERVICE_STATE_SHUTDOWN:
	case NM_VPN_SERVICE_STATE_STOPPING:
	case NM_VPN_SERVICE_STATE_STOPPED:
		remove_timeout_handler (NM_FORTISSLVPN_PLUGIN (object));
		g_clear_object (&priv->connection);
		priv->wait_2factor = FALSE;
		break;
	default:
		break;
	}
}

static void
dispose (GObject *object)
{
	NMFortisslvpnPluginPrivate *priv = NM_FORTISSLVPN_PLUGIN_GET_PRIVATE (object);
	GDBusInterfaceSkeleton *skeleton = NULL;

	if (priv->dbus_skeleton)
		skeleton = G_DBUS_INTERFACE_SKELETON (priv->dbus_skeleton);

	cleanup_plugin (NM_FORTISSLVPN_PLUGIN (object));

	if (skeleton) {
		if (g_dbus_interface_skeleton_get_object_path (skeleton))
			g_dbus_interface_skeleton_unexport (skeleton);
		g_signal_handlers_disconnect_by_func (skeleton, handle_set_state, object);
		g_signal_handlers_disconnect_by_func (skeleton, handle_set_ip4_config, object);
	}

	G_OBJECT_CLASS (nm_fortisslvpn_plugin_parent_class)->dispose (object);
}

static void
nm_fortisslvpn_plugin_init (NMFortisslvpnPlugin *plugin)
{
}

static void
nm_fortisslvpn_plugin_class_init (NMFortisslvpnPluginClass *fortisslvpn_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (fortisslvpn_class);
	NMVpnServicePluginClass *parent_class = NM_VPN_SERVICE_PLUGIN_CLASS (fortisslvpn_class);

	g_type_class_add_private (object_class, sizeof (NMFortisslvpnPluginPrivate));

	/* virtual methods */
	object_class->dispose = dispose;
	parent_class->connect = real_connect;
	parent_class->need_secrets = real_need_secrets;
	parent_class->disconnect = real_disconnect;
	parent_class->new_secrets = real_new_secrets;
}

static GInitableIface *ginitable_parent_iface = NULL;

static gboolean
init_sync (GInitable *object, GCancellable *cancellable, GError **error)
{
	NMFortisslvpnPluginPrivate *priv = NM_FORTISSLVPN_PLUGIN_GET_PRIVATE (object);
	GDBusConnection *connection;

	if (!ginitable_parent_iface->init (object, cancellable, error))
		return FALSE;

	g_signal_connect (G_OBJECT (object), "state-changed", G_CALLBACK (state_changed_cb), NULL);

	connection = nm_vpn_service_plugin_get_connection (NM_VPN_SERVICE_PLUGIN (object)),
	priv->dbus_skeleton = nmdbus_fortisslvpn_ppp_skeleton_new ();
	if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (priv->dbus_skeleton),
	                                       connection,
	                                       NM_DBUS_PATH_FORTISSLVPN_PPP,
	                                       error)) {
		g_prefix_error (error, "Failed to export helper interface: ");
		g_object_unref (connection);
		return FALSE;
	}

	g_dbus_connection_register_object (connection, NM_DBUS_PATH_FORTISSLVPN_PPP,
	                                   nmdbus_fortisslvpn_ppp_interface_info (),
	                                   NULL, NULL, NULL, NULL);

	g_signal_connect (priv->dbus_skeleton, "handle-set-state", G_CALLBACK (handle_set_state), object);
	g_signal_connect (priv->dbus_skeleton, "handle-set-ip4-config", G_CALLBACK (handle_set_ip4_config), object);

	g_object_unref (connection);
	return TRUE;
}

static void
nm_fortisslvpn_plugin_initable_iface_init (GInitableIface *iface)
{
	ginitable_parent_iface = g_type_interface_peek_parent (iface);
	iface->init = init_sync;
}

NMFortisslvpnPlugin *
nm_fortisslvpn_plugin_new (const char *bus_name)
{
	NMFortisslvpnPlugin *plugin;
	GError *error = NULL;

	plugin = (NMFortisslvpnPlugin *) g_initable_new (NM_TYPE_FORTISSLVPN_PLUGIN, NULL, &error,
	                                                 NM_VPN_SERVICE_PLUGIN_DBUS_SERVICE_NAME, bus_name,
	                                                 NM_VPN_SERVICE_PLUGIN_DBUS_WATCH_PEER, !debug,
	                                                 NULL);
	if (!plugin) {
		g_warning ("Failed to initialize a plugin instance: %s", error->message);
		g_error_free (error);
	}

	return plugin;
}

static void
quit_mainloop (NMFortisslvpnPlugin *plugin, gpointer user_data)
{
	g_main_loop_quit ((GMainLoop *) user_data);
}

int
main (int argc, char *argv[])
{
	NMFortisslvpnPlugin *plugin;
	GMainLoop *main_loop;
	gboolean persist = FALSE;
	GOptionContext *opt_ctx = NULL;
	gchar *bus_name = NM_DBUS_SERVICE_FORTISSLVPN;

	GOptionEntry options[] = {
		{ "persist", 0, 0, G_OPTION_ARG_NONE, &persist, N_("Don't quit when VPN connection terminates"), NULL },
		{ "debug", 0, 0, G_OPTION_ARG_NONE, &debug, N_("Enable verbose debug logging (may expose passwords)"), NULL },
		{ "bus-name", 0, 0, G_OPTION_ARG_STRING, &bus_name, N_("D-Bus name to use for this instance"), NULL },
		{NULL}
	};

#if !GLIB_CHECK_VERSION (2, 35, 0)
	g_type_init ();
#endif

	/* locale will be set according to environment LC_* variables */
	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, NM_FORTISSLVPN_LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	/* Parse options */
	opt_ctx = g_option_context_new (NULL);
	g_option_context_set_translation_domain (opt_ctx, GETTEXT_PACKAGE);
	g_option_context_set_ignore_unknown_options (opt_ctx, FALSE);
	g_option_context_set_help_enabled (opt_ctx, TRUE);
	g_option_context_add_main_entries (opt_ctx, options, NULL);

	g_option_context_set_summary (opt_ctx,
	    _("nm-fortisslvpn-service provides integrated SSLVPN capability (compatible with Fortinet) to NetworkManager."));

	g_option_context_parse (opt_ctx, &argc, &argv, NULL);
	g_option_context_free (opt_ctx);

	if (getenv ("NM_PPP_DEBUG"))
		debug = TRUE;

	if (debug)
		g_message ("nm-fortisslvpn-service (version " DIST_VERSION ") starting...");

	if (bus_name)
		setenv ("NM_DBUS_SERVICE_FORTISSLVPN", bus_name, 0);

	plugin = nm_fortisslvpn_plugin_new (bus_name);
	if (!plugin)
		exit (EXIT_FAILURE);

	main_loop = g_main_loop_new (NULL, FALSE);

	if (!persist)
		g_signal_connect (plugin, "quit", G_CALLBACK (quit_mainloop), main_loop);

	g_main_loop_run (main_loop);

	g_main_loop_unref (main_loop);
	g_object_unref (plugin);

	exit (EXIT_SUCCESS);
}
