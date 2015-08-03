/*
 * ConnMan GTK GUI
 *
 * Copyright (C) 2015 Intel Corporation. All rights reserved.
 * Author: Jaakko Hannikainen <jaakko.hannikainen@intel.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <openconnect.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "openconnect_helper.h"
#include "../src/util.h"

token_asker ask_tokens;
GString *progress;

static int invalid_cert(void *data, const char *reason)
{
	printf("%s\n", reason);
	return 0;
}

static int new_config(void *data, const char *buf, int buflen)
{
	return 0;
}

static void free_token(void *token) {
	struct auth_token *t = token;

	if(t->list)
		g_ptr_array_free(t->options, TRUE);
	g_free(t);
}

static int ask_pass(void *data, struct oc_auth_form *form)
{
	struct oc_form_opt *opt;
	GPtrArray *tokens;
	int i;

	tokens = g_ptr_array_new_full(0, free_token);

	for(opt = form->opts; opt; opt = opt->next) {
		struct auth_token *token = NULL;

		if(opt->flags & OC_FORM_OPT_IGNORE)
			continue;

		if(opt->type != OC_FORM_OPT_TOKEN)
			token = g_malloc0(sizeof(*token));

		switch(opt->type) {
		case OC_FORM_OPT_SELECT: {
			struct oc_form_opt_select *select;
			int i;

			select = (struct oc_form_opt_select *)opt;
			token->list = TRUE;
			token->label = select->form.label;
			token->options = g_ptr_array_new();
			for(i = 0; i < select->nr_choices; i++)
				g_ptr_array_add(token->options,
						select->choices[i]->label);
			break;
		}
		case OC_FORM_OPT_TEXT:
			if(!strcmp(opt->name, "username"))
				token->label = _("Username");
			else
				token->label = opt->name;
			break;
		case OC_FORM_OPT_PASSWORD:
			if(!strcmp(opt->name, "password"))
				token->label = _("Password");
			else
				token->label = opt->name;
			token->hidden = TRUE;
			break;
		case OC_FORM_OPT_TOKEN:
			break;
		}
		if(token)
			g_ptr_array_add(tokens, token);
	}

	if(!ask_tokens(tokens)) {
		g_ptr_array_free(tokens, TRUE);
		return OC_FORM_RESULT_CANCELLED;
	}

	i = 0;
	for(opt = form->opts; opt; opt = opt->next) {
		struct auth_token *token = NULL;

		if(opt->flags & OC_FORM_OPT_IGNORE)
			continue;

		switch(opt->type) {
		case OC_FORM_OPT_SELECT:
		case OC_FORM_OPT_TEXT:
		case OC_FORM_OPT_PASSWORD:
			token = tokens->pdata[i++];
			opt->_value = token->value;
			break;
		case OC_FORM_OPT_TOKEN:
			break;
		}
	}

	g_ptr_array_free(tokens, TRUE);

	return OC_FORM_RESULT_OK;
}

static void show_progress(void *data, int level, const char *fmt, ...)
{
	gchar *msg;
	va_list argp;

	va_start(argp, fmt);
	msg = g_strdup_vprintf(fmt, argp);
	va_end(argp);
	printf("%s", msg);
	g_string_append(progress, msg);
	g_free(msg);
}

static GVariantDict *get_tokens(GHashTable *info)
{
	GVariantDict *tokens = NULL;
	gchar *host, *cert;
	struct openconnect_info *vpninfo;
	int status;

	progress = g_string_new(NULL);
	openconnect_init_ssl();
	vpninfo = openconnect_vpninfo_new("linux-64", invalid_cert, new_config,
					  ask_pass, show_progress, NULL);

	host = g_hash_table_lookup(info, "Host");

	cert = g_hash_table_lookup(info, "OpenConnect.ClientCert");
	openconnect_set_client_cert(vpninfo, cert, NULL);

	// XXX: connman doesn't support configuring this
	openconnect_passphrase_from_fsid(vpninfo);

	openconnect_parse_url(vpninfo, host);

	status = openconnect_obtain_cookie(vpninfo);
	if(status) {
		if(status != OC_FORM_RESULT_CANCELLED)
			show_error(_("Connecting to VPN failed."),
				   progress->str);
		goto out;
	}

	tokens = g_variant_dict_new(NULL);

	g_variant_dict_insert(tokens, "OpenConnect.ServerCert", "s",
			      openconnect_get_peer_cert_hash(vpninfo));
	g_variant_dict_insert(tokens, "OpenConnect.Cookie", "s",
			      openconnect_get_cookie(vpninfo));
	g_variant_dict_insert(tokens, "OpenConnect.VPNHost", "s",
			      openconnect_get_hostname(vpninfo));

out:
	openconnect_vpninfo_free(vpninfo);
	g_string_free(progress, TRUE);
	return tokens;
}

GVariantDict *openconnect_handle(GDBusMethodInvocation *invocation,
				 GVariant *args, token_asker asker)
{
	GHashTable *info, *required;
	GVariantDict *out;
	GVariantIter *iter;
	gchar *key;
	GVariant *value;
	GVariant *parameters;

	ask_tokens = asker;
	parameters = g_variant_get_child_value(args, 1);

	info = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	required = g_hash_table_new_full(g_str_hash, g_str_equal,
					 g_free, g_free);

	iter = g_variant_iter_new(parameters);
	while(g_variant_iter_loop(iter, "{sv}", &key, &value)) {
		GVariantDict *dict = g_variant_dict_new(value);
		GVariant *req_v, *val_v;
		const gchar *req;
		gchar *val;

		req_v = g_variant_dict_lookup_value(dict, "Requirement", NULL);
		val_v = g_variant_dict_lookup_value(dict, "Value", NULL);
		req = g_variant_get_string(req_v, NULL);
		if(val_v) {
			val = g_variant_dup_string(val_v, NULL);
			if(!strcmp(req, "informational"))
				g_hash_table_insert(info, g_strdup(key), val);
			g_variant_unref(val_v);
		} else if(!strcmp(req, "mandatory"))
			g_hash_table_insert(required, g_strdup(key), NULL);
		g_variant_unref(req_v);
		g_variant_dict_unref(dict);
	}
	g_variant_iter_free(iter);

	out = get_tokens(info);

	g_hash_table_unref(required);
	g_hash_table_unref(info);

	return out;
}