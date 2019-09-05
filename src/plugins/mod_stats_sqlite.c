/*
 * uhub - A tiny ADC p2p connection hub
 * Copyright (C) 2007-2014, Jan Vidar Krey
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <inttypes.h>
#include <sqlite3.h>

#include "uhub.h"
#include "core/user.h"

#include "plugin_api/handle.h"
#include "util/config_token.h"
#include "util/memory.h"
#include "util/misc.h"
#include "util/list.h"
#include "util/cbuffer.h"

struct stats_data
{
	sqlite3* db;
};

static int null_callback(void* ptr, int argc, char **argv, char **colName) { return 0; }

static int sql_execute(struct stats_data* sql, int (*callback)(void* ptr, int argc, char **argv, char **colName), void* ptr, const char* sql_fmt, ...)
{
	va_list args;
	char query[1024];
	char* errMsg;
	int rc;

	va_start(args, sql_fmt);
	vsnprintf(query, sizeof(query), sql_fmt, args);

	rc = sqlite3_exec(sql->db, query, callback, ptr, &errMsg);
	if (rc != SQLITE_OK)
	{
		sqlite3_free(errMsg);
		return -rc;
	}

	rc = sqlite3_changes(sql->db);
	return rc;
}

static void create_tables(struct plugin_handle* plugin)
{
	const char* table_create = "CREATE TABLE IF NOT EXISTS user_stats"
		"("
			"cid CHAR PRIMARY KEY,"
			"logged INTEGER DEFAULT 0,"
			"shared_size CHAR DEFAULT '0',"
			"shared_files CHAR DEFAULT '0'"
		");";

	struct stats_data* data = (struct stats_data*) plugin->ptr;
	sql_execute(data, null_callback, NULL, table_create);
}

static void set_error_message(struct plugin_handle* plugin, const char* msg)
{
	plugin->error_msg = msg;
}

static struct stats_data* parse_config(const char* line, struct plugin_handle* plugin)
{
	struct stats_data* data = (struct stats_data*) hub_malloc_zero(sizeof(struct stats_data));
	struct cfg_tokens* tokens = cfg_tokenize(line);
	char* token = cfg_token_get_first(tokens);

	if (!data)
		return 0;

	while (token)
	{
		struct cfg_settings* setting = cfg_settings_split(token);

		if (!setting)
		{
			set_error_message(plugin, "Unable to parse startup parameters");
			cfg_tokens_free(tokens);
			hub_free(data);
			return 0;
		}

		if (strcmp(cfg_settings_get_key(setting), "file") == 0)
		{
			if (!data->db)
			{
				if (sqlite3_open(cfg_settings_get_value(setting), &data->db))
				{
					cfg_tokens_free(tokens);
					cfg_settings_free(setting);
					hub_free(data);
					set_error_message(plugin, "Unable to open database file");
					return 0;
				}
			}
		}
		else
		{
			set_error_message(plugin, "Unknown startup parameters given");
			cfg_tokens_free(tokens);
			cfg_settings_free(setting);
			hub_free(data);
			return 0;
		}

		cfg_settings_free(setting);
		token = cfg_token_get_next(tokens);
	}
	cfg_tokens_free(tokens);

	if (!data->db)
	{
	      set_error_message(plugin, "No database file is given, use file=<database>");
	      hub_free(data);
	      return 0;
	}
	return data;
}

static void sql_update_user(struct stats_data* data, struct plugin_user* user)
{
	struct hub_user* hub_user = (struct hub_user*) user;
	sql_execute(data, null_callback, NULL, "INSERT OR REPLACE INTO user_stats (cid, logged, shared_size, shared_files) VALUES('%s', 1, '%" PRIu64 "', '%zu');",
			user->cid,
			hub_user->limits.shared_size,
			hub_user->limits.shared_files);
}

static void on_user_login(struct plugin_handle* plugin, struct plugin_user* user)
{
	struct stats_data* data = (struct stats_data*) plugin->ptr;
	sql_update_user(data, user);
}

static void on_user_logout(struct plugin_handle* plugin, struct plugin_user* user, const char* reason)
{
	struct stats_data* data = (struct stats_data*) plugin->ptr;
	struct hub_user* hub_user = (struct hub_user*) user;
	sql_execute(data, null_callback, NULL, "INSERT OR REPLACE INTO user_stats (cid, logged, shared_size, shared_files) VALUES('%s', 0, '%" PRIu64 "', '%zu');",
			user->cid,
			hub_user->limits.shared_size,
			hub_user->limits.shared_files);
}

static plugin_st on_search(struct plugin_handle* plugin, struct plugin_user* from, const char* search)
{
	struct stats_data* data = (struct stats_data*) plugin->ptr;
	sql_update_user(data, from);
}

static plugin_st on_p2p_connect(struct plugin_handle* plugin, struct plugin_user* from, struct plugin_user* to)
{
	struct stats_data* data = (struct stats_data*) plugin->ptr;
	sql_update_user(data, from);
	sql_update_user(data, to);
}

int plugin_register(struct plugin_handle* plugin, const char* config)
{
	struct stats_data* data;
	PLUGIN_INITIALIZE(plugin, "SQLite hub stats plugin", "0.1", "Save hub statistics in a database.");

	plugin->funcs.on_user_login = on_user_login;
	plugin->funcs.on_user_logout = on_user_logout;
	plugin->funcs.on_search = on_search;
	plugin->funcs.on_p2p_connect = on_p2p_connect;

	data = parse_config(config, plugin);
	if (!data)
		return -1;

	plugin->ptr = data;
	create_tables(plugin);

	return 0;
}

int plugin_unregister(struct plugin_handle* plugin)
{
	struct stats_data* data = (struct stats_data*) plugin->ptr;
	if (data)
	{
		sqlite3_close(data->db);
		hub_free(data);
	}

	return 0;
}
