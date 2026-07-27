/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2025, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * switch_core_recovery.c -- Track and recover channels across a restart or a node handover
 *
 * A tracked channel has its xml cdr stored in the recovery table and published in the
 * SWITCH_RECOVERY_EVENT event, so that an external system may hold it as well. Recovery
 * reads those xml cdrs back, either from the table (switch_core_recovery_recover) or from
 * the SWITCH_RECOVERY_EXTERNAL_EVENT event fired by another node (handled here), and
 * resurrects the channels they describe.
 *
 */

#include <switch.h>
#include "private/switch_core_pvt.h"

#define RECOVERY_DEFAULT_QUEUE_SIZE 5000
#define RECOVERY_DEFAULT_WORKERS 1
#define RECOVERY_MAX_WORKERS 32

static struct {
	switch_queue_t *queue;
	switch_thread_t *workers[RECOVERY_MAX_WORKERS];
	uint32_t worker_count;
	uint32_t queue_size;
	int running;
} recovery_manager;

/*! Tracking writes to the recovery table unless recovery-use-db says otherwise, and never without a database */
static switch_bool_t recovery_use_db(void)
{
	return (runtime.recovery_use_db && switch_test_flag((&runtime), SCF_USE_SQL)) ? SWITCH_TRUE : SWITCH_FALSE;
}


SWITCH_DECLARE(void) switch_core_recovery_flush(const char *technology, const char *profile_name)
{
	char *sql = NULL;
	switch_cache_db_handle_t *dbh;

	if (switch_core_db_handle(&dbh) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error Opening DB!\n");
		return;
	}

	if (zstr(technology)) {

		if (zstr(profile_name)) {
			sql = switch_mprintf("delete from recovery");
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "INVALID\n");
		}

	} else {
		if (zstr(profile_name)) {
			sql = switch_mprintf("delete from recovery where technology='%q' ", technology);
		} else {
			sql = switch_mprintf("delete from recovery where technology='%q' and profile_name='%q'", technology, profile_name);
		}
	}

	if (sql) {
		switch_cache_db_execute_sql(dbh, sql, NULL);
		switch_safe_free(sql);
	}

	switch_cache_db_release_db_handle(&dbh);
}


/*!
  Recover one channel out of an event holding the columns of the recovery table:
  "technology" and "metadata". Fed either by switch_cache_db_execute_sql_event_callback(),
  which turns every row into such an event, or by recover_xml() for channels handed over
  by another node.
*/
static int recover_callback(void *pArg, switch_event_t *row)
{
	int *rp = (int *) pArg;
	const char *technology = switch_event_get_header(row, "technology");
	const char *xml_cdr_str = switch_event_get_header(row, "metadata");
	switch_xml_t xml;
	switch_endpoint_interface_t *ep;
	switch_core_session_t *session;

	if (zstr(technology) || zstr(xml_cdr_str)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Missing technology or metadata, call not recovered\n");
		return 0;
	}

	if (!(xml = switch_xml_parse_str_dynamic((char *) xml_cdr_str, SWITCH_TRUE))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "XML ERROR\n");
		return 0;
	}

	if (!(ep = switch_loadable_module_get_endpoint_interface(technology))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "EP ERROR\n");
		switch_xml_free(xml);
		return 0;
	}

	if (!(session = switch_core_session_request_xml(ep, NULL, xml))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Invalid cdr data, call not recovered\n");
		goto end;
	}

	if (ep->recover_callback) {
		switch_caller_extension_t *extension = NULL;
		switch_channel_t *channel = switch_core_session_get_channel(session);
		int r = 0;

		if ((r = ep->recover_callback(session)) > 0) {
			const char *cbname;

			switch_channel_set_flag(channel, CF_RECOVERING);

			if (switch_channel_get_partner_uuid(channel)) {
				switch_channel_set_flag(channel, CF_RECOVERING_BRIDGE);
			}

			switch_core_media_recover_session(session);

			if ((cbname = switch_channel_get_variable(channel, "secondary_recovery_module"))) {
				switch_core_recover_callback_t secondary_callback;

				if ((secondary_callback = switch_core_get_secondary_recover_callback(cbname))) {
					r = secondary_callback(session);
				}
			}
		}

		if (r > 0) {
			if (!switch_channel_test_flag(channel, CF_RECOVERING_BRIDGE)) {
				switch_xml_t callflow, param, x_extension;
				if ((extension = switch_caller_extension_new(session, "recovery", "recovery")) == 0) {
					abort();
				}

				if ((callflow = switch_xml_child(xml, "callflow")) && (x_extension = switch_xml_child(callflow, "extension"))) {
					int recovery_skip_announcement_type_applications = switch_channel_var_true(channel, "recovery_skip_announcement_type_applications");
					for (param = switch_xml_child(x_extension, "application"); param; param = param->next) {
						const char *var = switch_xml_attr_soft(param, "app_name");
						const char *val = switch_xml_attr_soft(param, "app_data");
						if (!recovery_skip_announcement_type_applications || (strcasecmp(var, "speak") && strcasecmp(var, "playback") && strcasecmp(var, "gentones") && strcasecmp(var, "say"))) {
							switch_caller_extension_add_application(session, extension, var, val);
						}
					}
				}

				switch_channel_set_caller_extension(channel, extension);
			}

			switch_channel_set_state(channel, CS_INIT);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE,
							  "Resurrecting fallen channel %s\n", switch_channel_get_name(channel));
			switch_core_session_thread_launch(session);

			*rp = (*rp) + 1;
		}

	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Endpoint %s has no recovery function\n", technology);
	}

 end:

	UNPROTECT_INTERFACE(ep);
	switch_xml_free(xml);

	return 0;
}

SWITCH_DECLARE(int) switch_core_recovery_recover(const char *technology, const char *profile_name)

{
	char *sql = NULL;
	char *errmsg = NULL;
	switch_cache_db_handle_t *dbh;
	int r = 0;

	if (!switch_test_flag((&runtime), SCF_USE_SQL)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "DATABASE NOT AVAIALBLE, REVCOVERY NOT POSSIBLE\n");
		return 0;
	}

	if (switch_core_db_handle(&dbh) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error Opening DB!\n");
		return 0;
	}

	if (zstr(technology)) {

		if (zstr(profile_name)) {
			sql = switch_mprintf("select technology, profile_name, hostname, uuid, metadata "
								 "from recovery where runtime_uuid!='%q'",
								 switch_core_get_uuid());
		} else {
			sql = switch_mprintf("select technology, profile_name, hostname, uuid, metadata "
								 "from recovery where runtime_uuid!='%q' and profile_name='%q'",
								 switch_core_get_uuid(), profile_name);
		}

	} else {

		if (zstr(profile_name)) {
			sql = switch_mprintf("select technology, profile_name, hostname, uuid, metadata "
								 "from recovery where technology='%q' and runtime_uuid!='%q'",
								 technology, switch_core_get_uuid());
		} else {
			sql = switch_mprintf("select technology, profile_name, hostname, uuid, metadata "
								 "from recovery where technology='%q' and runtime_uuid!='%q' and profile_name='%q'",
								 technology, switch_core_get_uuid(), profile_name);
		}
	}


	switch_cache_db_execute_sql_event_callback(dbh, sql, recover_callback, &r, &errmsg);

	if (errmsg) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "SQL ERR: [%s] %s\n", sql, errmsg);
		switch_safe_free(errmsg);
	}

	switch_safe_free(sql);

	if (zstr(technology)) {
		if (zstr(profile_name)) {
			sql = switch_mprintf("delete from recovery where runtime_uuid!='%q'",
								 switch_core_get_uuid());
		} else {
			sql = switch_mprintf("delete from recovery where runtime_uuid!='%q' and profile_name='%q'",
								 switch_core_get_uuid(), profile_name);
		}
	} else {
		if (zstr(profile_name)) {
			sql = switch_mprintf("delete from recovery where runtime_uuid!='%q' and technology='%q' ",
								 switch_core_get_uuid(), technology);
		} else {
			sql = switch_mprintf("delete from recovery where runtime_uuid!='%q' and technology='%q' and profile_name='%q'",
								 switch_core_get_uuid(), technology, profile_name);
		}
	}

	switch_cache_db_execute_sql(dbh, sql, NULL);
	switch_safe_free(sql);

	switch_cache_db_release_db_handle(&dbh);

	return r;

}

/*! Recover a single channel handed over by another node, blocks until the session exists */
static int recover_xml(const char *technology, const char *xml_cdr_str)
{
	switch_event_t *row;
	int r = 0;

	/* feed recover_callback() the very same shape it gets for a row of the recovery table */
	switch_event_create(&row, SWITCH_EVENT_CLONE);
	switch_event_add_header_string(row, SWITCH_STACK_BOTTOM, "technology", switch_str_nil(technology));
	switch_event_add_header_string(row, SWITCH_STACK_BOTTOM, "metadata", switch_str_nil(xml_cdr_str));

	recover_callback(&r, row);

	switch_event_destroy(&row);

	return r;
}

static void *SWITCH_THREAD_FUNC recovery_thread(switch_thread_t *thread, void *obj)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "External recovery worker started\n");

	while (recovery_manager.running == 1) {
		void *pop = NULL;
		switch_event_t *event;
		const char *technology, *uuid;

		if (switch_queue_pop(recovery_manager.queue, &pop) != SWITCH_STATUS_SUCCESS) {
			continue;
		}

		if (!pop) {
			break;
		}

		event = (switch_event_t *) pop;
		technology = switch_event_get_header(event, "Recovery-Technology");
		uuid = switch_event_get_header(event, "Unique-ID");

		if (recover_xml(technology, switch_event_get_body(event))) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Recovered external channel %s [%s]\n",
							  switch_str_nil(uuid), switch_str_nil(technology));
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to recover external channel %s [%s]\n",
							  switch_str_nil(uuid), switch_str_nil(technology));
		}

		switch_event_destroy(&event);
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "External recovery worker ended\n");

	return NULL;
}

static void ext_recovery_event_handler(switch_event_t *event)
{
	switch_event_t *dup = NULL;

	if (recovery_manager.running != 1) {
		return;
	}

	if (zstr(switch_event_get_header(event, "Recovery-Technology")) || zstr(switch_event_get_body(event))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "External recovery event without technology or xml cdr, ignoring\n");
		return;
	}

	/* recovering a session is way too slow to be done inline on the dispatch thread, hand it over to a worker */
	if (switch_event_dup(&dup, event) != SWITCH_STATUS_SUCCESS) {
		return;
	}

	if (switch_queue_trypush(recovery_manager.queue, dup) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "External recovery queue is full (%u), dropping event\n", recovery_manager.queue_size);
		switch_event_destroy(&dup);
	}
}

SWITCH_DECLARE(void) switch_core_recovery_untrack(switch_core_session_t *session, switch_bool_t force)
{
	char *sql = NULL;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_bool_t use_db = recovery_use_db();

	if (!use_db && !runtime.recovery_fire_events) {
		return;
	}

	if (!switch_channel_test_flag(channel, CF_ANSWERED) || switch_channel_get_state(channel) < CS_SOFT_EXECUTE) {
		return;
	}

	if (!switch_channel_test_flag(channel, CF_TRACKABLE)) {
		return;
	}

	if ((switch_channel_test_flag(channel, CF_RECOVERING))) {
		return;
	}

	if (switch_channel_test_flag(channel, CF_TRACKED) || force) {

		if (use_db) {
			if (force) {
				sql = switch_mprintf("delete from recovery where uuid='%q'", switch_core_session_get_uuid(session));

			} else {
				sql = switch_mprintf("delete from recovery where runtime_uuid='%q' and uuid='%q'",
									 switch_core_get_uuid(), switch_core_session_get_uuid(session));
			}

			switch_sql_queue_manager_push(switch_core_sqldb_qm(), sql, 3, SWITCH_FALSE);
		}

		if (runtime.recovery_fire_events) {
			switch_event_t *event;
			if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, SWITCH_RECOVERY_UNTRACK_EVENT) == SWITCH_STATUS_SUCCESS) {
				switch_channel_event_set_data(channel, event);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Recovery-Force", force ? "true" : "false");
				switch_event_fire(&event);
			}
		}

		switch_channel_clear_flag(channel, CF_TRACKED);
	}

}

SWITCH_DECLARE(void) switch_core_recovery_track(switch_core_session_t *session)
{
	switch_xml_t cdr = NULL;
	char *xml_cdr_text = NULL;
	char *sql = NULL;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	const char *profile_name;
	const char *technology;
	switch_bool_t use_db = recovery_use_db();

	if (!use_db && !runtime.recovery_fire_events) {
		return;
	}

	if (!switch_channel_test_flag(channel, CF_ANSWERED) || switch_channel_get_state(channel) < CS_SOFT_EXECUTE) {
		return;
	}

	if (switch_channel_test_flag(channel, CF_RECOVERING) || !switch_channel_test_flag(channel, CF_TRACKABLE)) {
		return;
	}

	profile_name = switch_channel_get_variable_dup(channel, "recovery_profile_name", SWITCH_FALSE, -1);
	technology = session->endpoint_interface->interface_name;

	if (switch_ivr_generate_xml_cdr(session, &cdr) == SWITCH_STATUS_SUCCESS) {
		xml_cdr_text = switch_xml_toxml_nolock(cdr, SWITCH_FALSE);
		switch_xml_free(cdr);
	}

	if (xml_cdr_text) {
		if (use_db) {
			if (switch_channel_test_flag(channel, CF_TRACKED)) {
				sql = switch_mprintf("update recovery set metadata='%q' where uuid='%q'",  xml_cdr_text, switch_core_session_get_uuid(session));
			} else {
				sql = switch_mprintf("insert into recovery (runtime_uuid, technology, profile_name, hostname, uuid, metadata) "
									 "values ('%q','%q','%q','%q','%q','%q')",
									 switch_core_get_uuid(), switch_str_nil(technology),
									 switch_str_nil(profile_name), switch_core_get_switchname(), switch_core_session_get_uuid(session), xml_cdr_text);
			}

			switch_sql_queue_manager_push(switch_core_sqldb_qm(), sql, 2, SWITCH_FALSE);
		}

		if (runtime.recovery_fire_events) {
			switch_event_t *event;
			if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, SWITCH_RECOVERY_EVENT) == SWITCH_STATUS_SUCCESS) {
				switch_channel_event_set_data(channel, event);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Recovery-Action",
					switch_channel_test_flag(channel, CF_TRACKED) ? "update" : "insert");
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Recovery-Technology", switch_str_nil(technology));
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Recovery-Profile", switch_str_nil(profile_name));
				switch_event_add_body(event, "%s", xml_cdr_text);
				switch_event_fire(&event);
			}
		}

		switch_safe_free(xml_cdr_text);
		switch_channel_set_flag(channel, CF_TRACKED);

	}

}

void switch_core_recovery_init(void)
{
	switch_threadattr_t *thd_attr;
	uint32_t i;

	memset(&recovery_manager, 0, sizeof(recovery_manager));

	recovery_manager.queue_size = runtime.recovery_queue_size ? runtime.recovery_queue_size : RECOVERY_DEFAULT_QUEUE_SIZE;
	recovery_manager.worker_count = runtime.recovery_worker_threads ? runtime.recovery_worker_threads : RECOVERY_DEFAULT_WORKERS;

	if (recovery_manager.worker_count > RECOVERY_MAX_WORKERS) {
		recovery_manager.worker_count = RECOVERY_MAX_WORKERS;
	}

	switch_event_reserve_subclass(SWITCH_RECOVERY_EVENT);
	switch_event_reserve_subclass(SWITCH_RECOVERY_UNTRACK_EVENT);
	switch_event_reserve_subclass(SWITCH_RECOVERY_EXTERNAL_EVENT);

	switch_queue_create(&recovery_manager.queue, recovery_manager.queue_size, runtime.memory_pool);
	recovery_manager.running = 1;

	switch_threadattr_create(&thd_attr, runtime.memory_pool);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);

	for (i = 0; i < recovery_manager.worker_count; i++) {
		switch_thread_create(&recovery_manager.workers[i], thd_attr, recovery_thread, NULL, runtime.memory_pool);
	}

	switch_event_bind("core_recovery", SWITCH_EVENT_CUSTOM, SWITCH_RECOVERY_EXTERNAL_EVENT, ext_recovery_event_handler, NULL);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
					  "Recovery tracking: db %s, events %s. Listening on %s, %u worker(s), queue size %u\n",
					  recovery_use_db() ? "on" : "off", runtime.recovery_fire_events ? "on" : "off",
					  SWITCH_RECOVERY_EXTERNAL_EVENT, recovery_manager.worker_count, recovery_manager.queue_size);
}

void switch_core_recovery_shutdown(void)
{
	void *pop = NULL;
	uint32_t i;

	if (!recovery_manager.running) {
		return;
	}

	switch_event_unbind_callback(ext_recovery_event_handler);

	recovery_manager.running = -1;

	for (i = 0; i < recovery_manager.worker_count; i++) {
		switch_queue_trypush(recovery_manager.queue, NULL);
	}

	switch_queue_interrupt_all(recovery_manager.queue);

	for (i = 0; i < recovery_manager.worker_count; i++) {
		if (recovery_manager.workers[i]) {
			switch_status_t st;
			switch_thread_join(&st, recovery_manager.workers[i]);
			recovery_manager.workers[i] = NULL;
		}
	}

	while (switch_queue_trypop(recovery_manager.queue, &pop) == SWITCH_STATUS_SUCCESS) {
		switch_event_t *event = (switch_event_t *) pop;
		switch_event_destroy(&event);
	}
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
