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

#define HA_TRACK_CALL_EVENT   "ha:track_call"
#define HA_UNTRACK_CALL_EVENT "ha:untrack_call"
#define HA_RECOVER_EVENT      "ha:recover"

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

/* Fetch rtp-ip and ext-rtp-ip for a sofia profile via xmlstatus API.
 * Both out-params are malloc'd; caller must free() them.
 * Falls back to local_ip_v4 / NULL if profile not found. */
static void get_sofia_profile_ips(const char *profile_name, char **out_rtp_ip, char **out_ext_rtp_ip)
{
	switch_stream_handle_t stream = { 0 };
	switch_xml_t xml = NULL, profile_info, node;
	char arg[512];

	*out_rtp_ip     = NULL;
	*out_ext_rtp_ip = NULL;

	if (!zstr(profile_name)) {
		SWITCH_STANDARD_STREAM(stream);
		switch_snprintf(arg, sizeof(arg), "xmlstatus profile %s", profile_name);

		if (switch_api_execute("sofia", arg, NULL, &stream) == SWITCH_STATUS_SUCCESS && !zstr(stream.data)) {
			if ((xml = switch_xml_parse_str_dynamic(stream.data, SWITCH_TRUE))) {
				/* <profile><profile-info><rtp-ip>, <ext-rtp-ip> */
				if ((profile_info = switch_xml_child(xml, "profile-info"))) {
					if ((node = switch_xml_child(profile_info, "rtp-ip")) && !zstr(node->txt)) {
						*out_rtp_ip = strdup(node->txt);
					}
					if ((node = switch_xml_child(profile_info, "ext-rtp-ip")) && !zstr(node->txt)) {
						*out_ext_rtp_ip = strdup(node->txt);
					}
				}
				switch_xml_free(xml);
			}
		}
		switch_safe_free(stream.data);
	}

	if (!*out_rtp_ip) {
		*out_rtp_ip = strdup(switch_str_nil(switch_core_get_variable("local_ip_v4")));
	}
}

static void patch_xml_rtp(switch_xml_t xml, const char *rtp_ip, const char *ext_rtp_ip, switch_port_t rtp_port)
{
	switch_xml_t variables, node;
	char port_str[16];

	if (!(variables = switch_xml_child(xml, "variables"))) return;

	switch_snprintf(port_str, sizeof(port_str), "%d", rtp_port);

	if ((node = switch_xml_child(variables, SWITCH_LOCAL_MEDIA_IP_VARIABLE))) {
		switch_xml_set_txt_d(node, rtp_ip);
	}
	if ((node = switch_xml_child(variables, SWITCH_LOCAL_MEDIA_PORT_VARIABLE))) {
		switch_xml_set_txt_d(node, port_str);
	}
	/* advertised_media_ip → used for adv_sdp_ip in SDP; use ext-rtp-ip if profile has NAT */
	if ((node = switch_xml_child(variables, SWITCH_ADVERTISED_MEDIA_IP_VARIABLE))) {
		switch_xml_set_txt_d(node, ext_rtp_ip ? ext_rtp_ip : rtp_ip);
	}
}

static void recover_event_handler(switch_event_t *event)
{
	const char *technology = switch_event_get_header(event, "Recovery-Technology");
	const char *xml_cdr    = switch_event_get_body(event);
	switch_xml_t xml, variables, node;
	switch_endpoint_interface_t *ep;
	switch_core_session_t *session;
	char *rtp_ip = NULL;
	char *ext_rtp_ip = NULL;
	switch_port_t rtp_port = 0;

	if (zstr(technology) || zstr(xml_cdr)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "ha:recover event missing Recovery-Technology or body, ignoring\n");
		return;
	}

	if (!(xml = switch_xml_parse_str_dynamic((char *) xml_cdr, SWITCH_TRUE))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "ha:recover XML parse error\n");
		return;
	}

	/* get profile name → rtp ip / ext-rtp-ip → free port, then patch the XML before session creation */
	{
		const char *profile_name = NULL;

		if ((variables = switch_xml_child(xml, "variables")) &&
			(node = switch_xml_child(variables, "recovery_profile_name")) && !zstr(node->txt)) {
			profile_name = node->txt;
		}
		get_sofia_profile_ips(profile_name, &rtp_ip, &ext_rtp_ip);
	}

	if ((rtp_port = switch_rtp_request_port(rtp_ip))) {
		patch_xml_rtp(xml, rtp_ip, ext_rtp_ip, rtp_port);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "ha:recover no free RTP ports on %s, recovering with original port\n", rtp_ip);
	}

	if (!(ep = switch_loadable_module_get_endpoint_interface(technology))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "ha:recover unknown technology: %s\n", technology);
		goto end;
	}

	if (!(session = switch_core_session_request_xml(ep, NULL, xml))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "ha:recover invalid CDR, call not recovered\n");
		if (rtp_port) switch_rtp_release_port(rtp_ip, rtp_port);
		goto end;
	}

	if (ep->recover_callback) {
		switch_channel_t *channel = switch_core_session_get_channel(session);
		int r;

		if ((r = ep->recover_callback(session)) > 0) {
			const char *cbname;

			switch_channel_set_flag(session->channel, CF_RECOVERING);

			if (switch_channel_get_partner_uuid(channel)) {
				switch_channel_set_flag(channel, CF_RECOVERING_BRIDGE);
			}

			switch_core_media_recover_session(session);

			if ((cbname = switch_channel_get_variable(channel, "secondary_recovery_module"))) {
				switch_core_recover_callback_t cb;

				if ((cb = switch_core_get_secondary_recover_callback(cbname))) {
					r = cb(session);
				}
			}
		}

		if (r > 0) {
			if (!switch_channel_test_flag(channel, CF_RECOVERING_BRIDGE)) {
				switch_caller_extension_t *extension;
				switch_xml_t callflow, param, x_extension;

				if ((extension = switch_caller_extension_new(session, "recovery", "recovery")) == 0) {
					abort();
				}

				if ((callflow = switch_xml_child(xml, "callflow")) && (x_extension = switch_xml_child(callflow, "extension"))) {
					int skip_ann = switch_channel_var_true(channel, "recovery_skip_announcement_type_applications");

					for (param = switch_xml_child(x_extension, "application"); param; param = param->next) {
						const char *var = switch_xml_attr_soft(param, "app_name");
						const char *val = switch_xml_attr_soft(param, "app_data");

						if (!skip_ann || (strcasecmp(var, "speak") && strcasecmp(var, "playback") &&
										  strcasecmp(var, "gentones") && strcasecmp(var, "say"))) {
							switch_caller_extension_add_application(session, extension, var, val);
						}
					}
				}

				switch_channel_set_caller_extension(channel, extension);
			}

			switch_channel_set_state(channel, CS_INIT);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE,
							  "ha:recover resurrecting %s\n", switch_channel_get_name(channel));
			switch_core_session_thread_launch(session);
		}

	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "ha:recover endpoint %s has no recover_callback\n", technology);
	}

 end:
	if (ep) UNPROTECT_INTERFACE(ep);
	switch_xml_free(xml);
	switch_safe_free(rtp_ip);
	switch_safe_free(ext_rtp_ip);
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

static void untrack_event_handler(switch_event_t *event)
{
	const char *uuid = switch_event_get_header(event, "Unique-ID");
	switch_core_session_t *session;
	switch_bool_t was_tracked;

	if (!uuid) return;

	switch_mutex_lock(globals.mutex);
	was_tracked = switch_core_hash_find(globals.tracked_calls, uuid) != NULL;
	if (was_tracked) {
		switch_core_hash_delete(globals.tracked_calls, uuid);
	}
	switch_mutex_unlock(globals.mutex);

	if (!was_tracked) return;

	if ((session = switch_core_session_locate(uuid))) {
		switch_channel_t *channel = switch_core_session_get_channel(session);
		switch_event_t *ha_event;

		if (switch_event_create_subclass(&ha_event, SWITCH_EVENT_CUSTOM, HA_UNTRACK_CALL_EVENT) == SWITCH_STATUS_SUCCESS) {
			switch_channel_event_set_data(channel, ha_event);
			switch_event_fire(&ha_event);
		}

		switch_core_session_rwunlock(session);
	} else {
		/* session already gone — fire event with just the UUID */
		switch_event_t *ha_event;

		if (switch_event_create_subclass(&ha_event, SWITCH_EVENT_CUSTOM, HA_UNTRACK_CALL_EVENT) == SWITCH_STATUS_SUCCESS) {
			switch_event_add_header_string(ha_event, SWITCH_STACK_BOTTOM, "Unique-ID", uuid);
			switch_event_fire(&ha_event);
		}
	}
}

static void destroy_event_handler(switch_event_t *event)
{
	/* safety net: clean up hash if hangup handler was missed */
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

	if (switch_event_reserve_subclass(HA_UNTRACK_CALL_EVENT) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to reserve subclass %s\n", HA_UNTRACK_CALL_EVENT);
		switch_event_free_subclass(HA_TRACK_CALL_EVENT);
		return SWITCH_STATUS_GENERR;
	}

	if (switch_event_reserve_subclass(HA_RECOVER_EVENT) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to reserve subclass %s\n", HA_RECOVER_EVENT);
		switch_event_free_subclass(HA_TRACK_CALL_EVENT);
		switch_event_free_subclass(HA_UNTRACK_CALL_EVENT);
		return SWITCH_STATUS_GENERR;
	}

	switch_event_bind(modname, SWITCH_EVENT_CHANNEL_ANSWER,   SWITCH_EVENT_SUBCLASS_ANY, track_event_handler,   NULL);
	switch_event_bind(modname, SWITCH_EVENT_CHANNEL_BRIDGE,   SWITCH_EVENT_SUBCLASS_ANY, track_event_handler,   NULL);
	switch_event_bind(modname, SWITCH_EVENT_CHANNEL_UNBRIDGE, SWITCH_EVENT_SUBCLASS_ANY, track_event_handler,   NULL);
	switch_event_bind(modname, SWITCH_EVENT_CHANNEL_HANGUP,   SWITCH_EVENT_SUBCLASS_ANY, untrack_event_handler, NULL);
	switch_event_bind(modname, SWITCH_EVENT_CHANNEL_DESTROY,  SWITCH_EVENT_SUBCLASS_ANY, destroy_event_handler, NULL);
	switch_event_bind(modname, SWITCH_EVENT_CUSTOM, HA_RECOVER_EVENT, recover_event_handler, NULL);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_ha loaded\n");

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_ha_shutdown)
{
	switch_event_unbind_callback(track_event_handler);
	switch_event_unbind_callback(untrack_event_handler);
	switch_event_unbind_callback(destroy_event_handler);
	switch_event_unbind_callback(recover_event_handler);
	switch_event_free_subclass(HA_TRACK_CALL_EVENT);
	switch_event_free_subclass(HA_UNTRACK_CALL_EVENT);
	switch_event_free_subclass(HA_RECOVER_EVENT);
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
