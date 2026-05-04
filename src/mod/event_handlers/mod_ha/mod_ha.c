/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
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
 * Alexey Khabulyak <alexey.khabulyak@ooma.com>
 *
 *
 * mod_ha.c -- Remote call recovery module
 *
 */
#include <switch.h>

SWITCH_MODULE_LOAD_FUNCTION(mod_ha_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ha_shutdown);
SWITCH_MODULE_DEFINITION(mod_ha, mod_ha_load, mod_ha_shutdown, NULL);

#define HA_TRACK_CALL_EVENT "ha:track_call"

static struct {
	switch_hash_t  *tracked_calls;
	switch_mutex_t *mutex;
} globals;

/* Extract technology from channel name: "sofia/internal/..." -> "sofia" */
static void parse_technology(const char *channel_name, char *buf, switch_size_t len)
{
	const char *slash = channel_name ? strchr(channel_name, '/') : NULL;

	if (slash && (switch_size_t)(slash - channel_name) < len) {
		switch_copy_string(buf, channel_name, (slash - channel_name) + 1);
	} else {
		switch_copy_string(buf, channel_name ? channel_name : "", len);
	}
}

static void do_track(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_xml_t cdr = NULL;
	char *xml_cdr_text = NULL;
	switch_event_t *event;
	const char *uuid;
	const char *profile_name;
	char technology[64] = "";
	switch_bool_t is_update;

	if (!switch_channel_test_flag(channel, CF_ANSWERED) ||
		switch_channel_get_state(channel) < CS_SOFT_EXECUTE) {
		return;
	}

	/* skip calls that are themselves recovering, and non-trackable endpoints */
	if (switch_channel_test_flag(channel, CF_RECOVERING) ||
		!switch_channel_test_flag(channel, CF_TRACKABLE)) {
		return;
	}

	uuid = switch_core_session_get_uuid(session);
	profile_name = switch_channel_get_variable_dup(channel, "recovery_profile_name", SWITCH_FALSE, -1);
	parse_technology(switch_channel_get_name(channel), technology, sizeof(technology));

	if (switch_ivr_generate_xml_cdr(session, &cdr) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
						  "Failed to generate XML CDR for %s\n", uuid);
		return;
	}

	xml_cdr_text = switch_xml_toxml_nolock(cdr, SWITCH_FALSE);
	switch_xml_free(cdr);

	if (!xml_cdr_text) {
		return;
	}

	switch_mutex_lock(globals.mutex);
	is_update = switch_core_hash_find(globals.tracked_calls, uuid) != NULL;
	if (!is_update) {
		switch_core_hash_insert(globals.tracked_calls, uuid, (void *)(intptr_t)1);
	}
	switch_mutex_unlock(globals.mutex);

	if (switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, HA_TRACK_CALL_EVENT) == SWITCH_STATUS_SUCCESS) {
		switch_channel_event_set_data(channel, event);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Recovery-Action", is_update ? "update" : "insert");
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Recovery-Technology", technology);
		switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Recovery-Profile", switch_str_nil(profile_name));
		switch_event_add_body(event, "%s", xml_cdr_text);
		switch_event_fire(&event);
	}

	switch_safe_free(xml_cdr_text);
}

static void track_event_handler(switch_event_t *event)
{
	const char *uuid = switch_event_get_header(event, "Unique-ID");
	switch_core_session_t *session;

	if (!uuid) return;

	if ((session = switch_core_session_locate(uuid))) {
		do_track(session);
		switch_core_session_rwunlock(session);
	}
}

static void destroy_event_handler(switch_event_t *event)
{
	const char *uuid = switch_event_get_header(event, "Unique-ID");

	if (!uuid) return;

	switch_mutex_lock(globals.mutex);
	switch_core_hash_delete(globals.tracked_calls, uuid);
	switch_mutex_unlock(globals.mutex);
}

SWITCH_MODULE_LOAD_FUNCTION(mod_ha_load)
{
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	switch_core_hash_init(&globals.tracked_calls);
	switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, pool);

	if (switch_event_reserve_subclass(HA_TRACK_CALL_EVENT) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to reserve subclass %s\n", HA_TRACK_CALL_EVENT);
		return SWITCH_STATUS_GENERR;
	}

	switch_event_bind(modname, SWITCH_EVENT_CHANNEL_ANSWER,   SWITCH_EVENT_SUBCLASS_ANY, track_event_handler,   NULL);
	switch_event_bind(modname, SWITCH_EVENT_CHANNEL_BRIDGE,   SWITCH_EVENT_SUBCLASS_ANY, track_event_handler,   NULL);
	switch_event_bind(modname, SWITCH_EVENT_CHANNEL_UNBRIDGE, SWITCH_EVENT_SUBCLASS_ANY, track_event_handler,   NULL);
	switch_event_bind(modname, SWITCH_EVENT_CHANNEL_DESTROY,  SWITCH_EVENT_SUBCLASS_ANY, destroy_event_handler, NULL);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_ha loaded\n");

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ha_shutdown)
{
	switch_event_unbind_callback(track_event_handler);
	switch_event_unbind_callback(destroy_event_handler);
	switch_event_free_subclass(HA_TRACK_CALL_EVENT);
	switch_core_hash_destroy(&globals.tracked_calls);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_ha unloaded\n");

	return SWITCH_STATUS_SUCCESS;
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
