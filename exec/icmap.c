/*
 * Copyright (c) 2011 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Jan Friesse (jfriesse@redhat.com)
 *
 * This software licensed under BSD license, the text of which follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the Red Hat, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>

#include <string.h>
#include <stdio.h>

#include <corosync/corotypes.h>

#include <qb/qbdefs.h>
#include <corosync/list.h>
#include <corosync/icmap.h>

#define ICMAP_MAX_VALUE_LEN	(16*1024)

struct icmap_item {
	char *key_name;
	icmap_value_types_t type;
	size_t value_len;
	char value[];
};

static qb_map_t *icmap_map;

struct icmap_track {
	char *key_name;
	int32_t track_type;
	icmap_notify_fn_t notify_fn;
	void *user_data;
};

struct icmap_ro_access_item {
	char *key_name;
	int prefix;
	struct list_head list;
};

DECLARE_LIST_INIT(icmap_ro_access_item_list_head);

/*
 * Static functions declarations
 */

/*
 * Check if key_name is valid icmap key name. Returns 0 on success, and -1 on fail
 */
static int icmap_check_key_name(const char *key_name);

/*
 * Check that value with given type has correct length value_len. Returns 0 on success,
 * and -1 on fail
 */
static int icmap_check_value_len(const void *value, size_t value_len, icmap_value_types_t type);

/*
 * Returns length of value of given type, or 0 for string and binary data type
 */
static size_t icmap_get_valuetype_len(icmap_value_types_t type);

/*
 * Converts track type of icmap to qb
 */
static int32_t icmap_tt_to_qbtt(int32_t track_type);

/*
 * Convert track type of qb to icmap
 */
static int32_t icmap_qbtt_to_tt(int32_t track_type);

/*
 * Checks if item has same value as value with value_len and given type. Returns 0 if not, otherwise !0.
 */
static int icmap_item_eq(const struct icmap_item *item, const void *value, size_t value_len, icmap_value_types_t type);

/*
 * Checks if given character is valid in key name. Returns 0 if not, otherwise !0.
 */
static int icmap_is_valid_name_char(char c);

/*
 * Helper for getting integer and float value with given type for key key_name and store it in value.
 */
static cs_error_t icmap_get_int(
	const char *key_name,
	void *value,
	icmap_value_types_t type);

/*
 * Function implementation
 */
static int32_t icmap_tt_to_qbtt(int32_t track_type)
{
	int32_t res = 0;

	if (track_type & ICMAP_TRACK_DELETE) {
		res |= QB_MAP_NOTIFY_DELETED;
	}

	if (track_type & ICMAP_TRACK_MODIFY) {
		res |= QB_MAP_NOTIFY_REPLACED;
	}

	if (track_type & ICMAP_TRACK_ADD) {
		res |= QB_MAP_NOTIFY_INSERTED;
	}

	if (track_type & ICMAP_TRACK_PREFIX) {
		res |= QB_MAP_NOTIFY_RECURSIVE;
	}

	return (track_type);
}

static int32_t icmap_qbtt_to_tt(int32_t track_type)
{
	int32_t res = 0;

	if (track_type & QB_MAP_NOTIFY_DELETED) {
		res |= ICMAP_TRACK_DELETE;
	}

	if (track_type & QB_MAP_NOTIFY_REPLACED) {
		res |= ICMAP_TRACK_MODIFY;
	}

	if (track_type & QB_MAP_NOTIFY_INSERTED) {
		res |= ICMAP_TRACK_ADD;
	}

	if (track_type & QB_MAP_NOTIFY_RECURSIVE) {
		res |= ICMAP_TRACK_PREFIX;
	}

	return (track_type);
}

static void icmap_map_free_cb(uint32_t event,
		char* key, void* old_value,
		void* value, void* user_data)
{
	struct icmap_item *item = (struct icmap_item *)old_value;

	if (item != NULL) {
		free(item->key_name);
		free(item);
	}
}

cs_error_t icmap_init(void)
{
	int32_t err;

	icmap_map = qb_trie_create();
	if (icmap_map == NULL)
		return (CS_ERR_INIT);

	err = qb_map_notify_add(icmap_map, NULL, icmap_map_free_cb, QB_MAP_NOTIFY_FREE, NULL);

	return (qb_to_cs_error(err));
}

static int icmap_is_valid_name_char(char c)
{
	return ((c >= 'a' && c <= 'z') ||
		(c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') ||
		c == '.' || c == '_' || c == '-' || c == '/' || c == ':');
}

void icmap_convert_name_to_valid_name(char *key_name)
{
	int i;

	for (i = 0; i < strlen(key_name); i++) {
		if (!icmap_is_valid_name_char(key_name[i])) {
			key_name[i] = '_';
		}
	}
}

static int icmap_check_key_name(const char *key_name)
{
	int i;

	if ((strlen(key_name) < ICMAP_KEYNAME_MINLEN) || strlen(key_name) > ICMAP_KEYNAME_MAXLEN) {
		return (-1);
	}

	for (i = 0; i < strlen(key_name); i++) {
		if (!icmap_is_valid_name_char(key_name[i])) {
			return (-1);
		}
	}

	return (0);
}

static size_t icmap_get_valuetype_len(icmap_value_types_t type)
{
	size_t res;

	switch (type) {
	case ICMAP_VALUETYPE_INT8: res = sizeof(int8_t); break;
	case ICMAP_VALUETYPE_UINT8: res = sizeof(uint8_t); break;
	case ICMAP_VALUETYPE_INT16: res = sizeof(int16_t); break;
	case ICMAP_VALUETYPE_UINT16: res = sizeof(uint16_t); break;
	case ICMAP_VALUETYPE_INT32: res = sizeof(int32_t); break;
	case ICMAP_VALUETYPE_UINT32: res = sizeof(uint32_t); break;
	case ICMAP_VALUETYPE_INT64: res = sizeof(int64_t); break;
	case ICMAP_VALUETYPE_UINT64: res = sizeof(uint64_t); break;
	case ICMAP_VALUETYPE_FLOAT: res = sizeof(float); break;
	case ICMAP_VALUETYPE_DOUBLE: res = sizeof(double); break;
	case ICMAP_VALUETYPE_STRING:
	case ICMAP_VALUETYPE_BINARY:
		res = 0;
		break;
	}

	return (res);
}

static int icmap_check_value_len(const void *value, size_t value_len, icmap_value_types_t type)
{

	if (value_len > ICMAP_MAX_VALUE_LEN) {
		return (-1);
	}

	if (type != ICMAP_VALUETYPE_STRING && type != ICMAP_VALUETYPE_BINARY) {
		if (icmap_get_valuetype_len(type) == value_len) {
			return (0);
		} else {
			return (-1);
		}
	}

	if (type == ICMAP_VALUETYPE_STRING) {
		if (value_len > strlen((const char *)value)) {
			return (-1);
		} else {
			return (0);
		}
	}

	return (0);
}

static int icmap_item_eq(const struct icmap_item *item, const void *value, size_t value_len, icmap_value_types_t type)
{
	size_t ptr_len;

	if (item->type != type) {
		return (0);
	}

	if (item->type == ICMAP_VALUETYPE_STRING) {
		ptr_len = strlen((const char *)value);
		if (ptr_len > value_len) {
			ptr_len = value_len;
		}
		ptr_len++;
	} else {
		ptr_len = value_len;
	}

	if (item->value_len == ptr_len) {
		return (memcmp(item->value, value, value_len) == 0);
	};

	return (0);
}

cs_error_t icmap_set(
	const char *key_name,
	const void *value,
	size_t value_len,
	icmap_value_types_t type)
{
	struct icmap_item *item;
	struct icmap_item *new_item;
	size_t new_value_len;
	size_t new_item_size;

	if (value == NULL || key_name == NULL) {
		return (CS_ERR_INVALID_PARAM);
	}

	if (icmap_check_value_len(value, value_len, type) != 0) {
		return (CS_ERR_INVALID_PARAM);
	}

	item = qb_map_get(icmap_map, key_name);
	if (item != NULL) {
		/*
		 * Check that key is really changed
		 */
		if (icmap_item_eq(item, value, value_len, type)) {
			return (CS_OK);
		}
	} else {
		if (icmap_check_key_name(key_name) != 0) {
			return (CS_ERR_NAME_TOO_LONG);
		}
	}

	if (type == ICMAP_VALUETYPE_BINARY || type == ICMAP_VALUETYPE_STRING) {
		if (type == ICMAP_VALUETYPE_STRING) {
			new_value_len = strlen((const char *)value);
			if (new_value_len > value_len) {
				new_value_len = value_len;
			}
			new_value_len++;
		} else {
			new_value_len = value_len;
		}
	} else {
		new_value_len = icmap_get_valuetype_len(type);
	}

	new_item_size = sizeof(struct icmap_item) + new_value_len;
	new_item = malloc(new_item_size);
	if (new_item == NULL) {
		return (CS_ERR_NO_MEMORY);
	}
	memset(new_item, 0, new_item_size);

	if (item == NULL) {
		new_item->key_name = strdup(key_name);
		if (new_item->key_name == NULL) {
			free(new_item);
			return (CS_ERR_NO_MEMORY);
		}
	} else {
		new_item->key_name = item->key_name;
		item->key_name = NULL;
	}

	new_item->type = type;
	new_item->value_len = new_value_len;

	memcpy(new_item->value, value, new_value_len);

	if (new_item->type == ICMAP_VALUETYPE_STRING) {
		((char *)new_item->value)[new_value_len - 1] = 0;
	}

	qb_map_put(icmap_map, new_item->key_name, new_item);

	return (CS_OK);
}

cs_error_t icmap_set_int8(const char *key_name, int8_t value)
{

	return (icmap_set(key_name, &value, sizeof(value), ICMAP_VALUETYPE_INT8));
}

cs_error_t icmap_set_uint8(const char *key_name, uint8_t value)
{

	return (icmap_set(key_name, &value, sizeof(value), ICMAP_VALUETYPE_UINT8));
}

cs_error_t icmap_set_int16(const char *key_name, int16_t value)
{

	return (icmap_set(key_name, &value, sizeof(value), ICMAP_VALUETYPE_INT16));
}

cs_error_t icmap_set_uint16(const char *key_name, uint16_t value)
{

	return (icmap_set(key_name, &value, sizeof(value), ICMAP_VALUETYPE_UINT16));
}

cs_error_t icmap_set_int32(const char *key_name, int32_t value)
{

	return (icmap_set(key_name, &value, sizeof(value), ICMAP_VALUETYPE_INT32));
}

cs_error_t icmap_set_uint32(const char *key_name, uint32_t value)
{

	return (icmap_set(key_name, &value, sizeof(value), ICMAP_VALUETYPE_UINT32));
}

cs_error_t icmap_set_int64(const char *key_name, int64_t value)
{

	return (icmap_set(key_name, &value, sizeof(value), ICMAP_VALUETYPE_INT64));
}

cs_error_t icmap_set_uint64(const char *key_name, uint64_t value)
{

	return (icmap_set(key_name, &value, sizeof(value), ICMAP_VALUETYPE_UINT64));
}

cs_error_t icmap_set_float(const char *key_name, float value)
{

	return (icmap_set(key_name, &value, sizeof(value), ICMAP_VALUETYPE_FLOAT));
}

cs_error_t icmap_set_double(const char *key_name, double value)
{

	return (icmap_set(key_name, &value, sizeof(value), ICMAP_VALUETYPE_DOUBLE));
}

cs_error_t icmap_set_string(const char *key_name, const char *value)
{

	return (icmap_set(key_name, value, strlen(value), ICMAP_VALUETYPE_STRING));
}

cs_error_t icmap_delete(const char *key_name)
{
	struct icmap_item *item;

	if (key_name == NULL) {
		return (CS_ERR_INVALID_PARAM);
	}

	item = qb_map_get(icmap_map, key_name);
	if (item == NULL) {
		return (CS_ERR_NOT_EXIST);
	}

	if (qb_map_rm(icmap_map, item->key_name) != QB_TRUE) {
		return (CS_ERR_NOT_EXIST);
	}

	return (CS_OK);
}

cs_error_t icmap_get(
	const char *key_name,
	void *value,
	size_t *value_len,
	icmap_value_types_t *type)
{
	struct icmap_item *item;

	if (key_name == NULL) {
		return (CS_ERR_INVALID_PARAM);
	}

	item = qb_map_get(icmap_map, key_name);
	if (item == NULL) {
		return (CS_ERR_NOT_EXIST);
	}

	if (type != NULL) {
		*type = item->type;
	}

	if (value == NULL) {
		if (value_len != NULL) {
			*value_len = item->value_len;
		}
	} else {
		if (value_len == NULL || *value_len < item->value_len) {
			return (CS_ERR_INVALID_PARAM);
		}

		*value_len = item->value_len;

		memcpy(value, item->value, item->value_len);
	}

	return (CS_OK);
}

static cs_error_t icmap_get_int(
	const char *key_name,
	void *value,
	icmap_value_types_t type)
{
	char key_value[16];
	size_t key_size;
	cs_error_t err;
	icmap_value_types_t key_type;

	key_size = sizeof(key_value);
	memset(key_value, 0, key_size);

	err = icmap_get(key_name, key_value, &key_size, &key_type);
	if (err != CS_OK)
		return (err);

	if (key_type != type) {
		return (CS_ERR_INVALID_PARAM);
	}

	memcpy(value, key_value, icmap_get_valuetype_len(key_type));

	return (CS_OK);
}

cs_error_t icmap_get_int8(const char *key_name, int8_t *i8)
{

	return (icmap_get_int(key_name, i8, ICMAP_VALUETYPE_INT8));
}

cs_error_t icmap_get_uint8(const char *key_name, uint8_t *u8)
{

	return (icmap_get_int(key_name, u8, ICMAP_VALUETYPE_UINT8));
}

cs_error_t icmap_get_int16(const char *key_name, int16_t *i16)
{

	return (icmap_get_int(key_name, i16, ICMAP_VALUETYPE_INT16));
}

cs_error_t icmap_get_uint16(const char *key_name, uint16_t *u16)
{

	return (icmap_get_int(key_name, u16, ICMAP_VALUETYPE_UINT16));
}

cs_error_t icmap_get_int32(const char *key_name, int32_t *i32)
{

	return (icmap_get_int(key_name, i32, ICMAP_VALUETYPE_INT32));
}

cs_error_t icmap_get_uint32(const char *key_name, uint32_t *u32)
{

	return (icmap_get_int(key_name, u32, ICMAP_VALUETYPE_UINT32));
}

cs_error_t icmap_get_int64(const char *key_name, int64_t *i64)
{

	return(icmap_get_int(key_name, i64, ICMAP_VALUETYPE_INT64));
}

cs_error_t icmap_get_uint64(const char *key_name, uint64_t *u64)
{

	return (icmap_get_int(key_name, u64, ICMAP_VALUETYPE_UINT64));
}

cs_error_t icmap_get_float(const char *key_name, float *flt)
{

	return (icmap_get_int(key_name, flt, ICMAP_VALUETYPE_FLOAT));
}

cs_error_t icmap_get_double(const char *key_name, double *dbl)
{

	return (icmap_get_int(key_name, dbl, ICMAP_VALUETYPE_DOUBLE));
}

cs_error_t icmap_get_string(const char *key_name, char **str)
{
	cs_error_t res;
	size_t str_len;
	icmap_value_types_t type;

	res = icmap_get(key_name, NULL, &str_len, &type);
	if (res != CS_OK || type != ICMAP_VALUETYPE_STRING) {
		if (res == CS_OK) {
			res = CS_ERR_INVALID_PARAM;
		}

		goto return_error;
	}

	*str = malloc(str_len);
	if (*str == NULL) {
		res = CS_ERR_NO_MEMORY;

		goto return_error;
	}

	res = icmap_get(key_name, *str, &str_len, &type);
	if (res != CS_OK) {
		free(*str);
		goto return_error;
	}

	return (CS_OK);

return_error:
	return (res);
}

cs_error_t icmap_adjust_int(
	const char *key_name,
	int32_t step)
{
	struct icmap_item *item;
	uint8_t u8;
	uint16_t u16;
	uint32_t u32;
	uint64_t u64;
	cs_error_t err = CS_OK;

	if (key_name == NULL) {
		return (CS_ERR_INVALID_PARAM);
	}

	item = qb_map_get(icmap_map, key_name);
	if (item == NULL) {
		return (CS_ERR_NOT_EXIST);
	}

	switch (item->type) {
	case ICMAP_VALUETYPE_INT8:
	case ICMAP_VALUETYPE_UINT8:
		memcpy(&u8, item->value, sizeof(u8));
		u8 += step;
		err = icmap_set(key_name, &u8, sizeof(u8), item->type);
		break;
	case ICMAP_VALUETYPE_INT16:
	case ICMAP_VALUETYPE_UINT16:
		memcpy(&u16, item->value, sizeof(u16));
		u16 += step;
		err = icmap_set(key_name, &u16, sizeof(u16), item->type);
		break;
	case ICMAP_VALUETYPE_INT32:
	case ICMAP_VALUETYPE_UINT32:
		memcpy(&u32, item->value, sizeof(u32));
		u32 += step;
		err = icmap_set(key_name, &u32, sizeof(u32), item->type);
		break;
	case ICMAP_VALUETYPE_INT64:
	case ICMAP_VALUETYPE_UINT64:
		memcpy(&u64, item->value, sizeof(u64));
		u64 += step;
		err = icmap_set(key_name, &u64, sizeof(u64), item->type);
		break;
	case ICMAP_VALUETYPE_FLOAT:
	case ICMAP_VALUETYPE_DOUBLE:
	case ICMAP_VALUETYPE_STRING:
	case ICMAP_VALUETYPE_BINARY:
		err = CS_ERR_INVALID_PARAM;
		break;
	}

	return (err);
}

cs_error_t icmap_inc(const char *key_name)
{
	return (icmap_adjust_int(key_name, 1));
}

cs_error_t icmap_dec(const char *key_name)
{
	return (icmap_adjust_int(key_name, -1));
}

icmap_iter_t icmap_iter_init(const char *prefix)
{
	return (qb_map_pref_iter_create(icmap_map, prefix));
}

const char *icmap_iter_next(icmap_iter_t iter, size_t *value_len, icmap_value_types_t *type)
{
	struct icmap_item *item;
	const char *res;

	res = qb_map_iter_next(iter, (void **)&item);
	if (res == NULL) {
		return (res);
	}

	if (value_len != NULL) {
		*value_len = item->value_len;
	}

	if (type != NULL) {
		*type = item->type;
	}

	return (res);
}

void icmap_iter_finalize(icmap_iter_t iter)
{
	qb_map_iter_free(iter);
}

static void icmap_notify_fn(uint32_t event, char *key, void *old_value, void *value, void *user_data)
{
	icmap_track_t icmap_track = (icmap_track_t)user_data;
	struct icmap_item *new_item = (struct icmap_item *)value;
	struct icmap_item *old_item = (struct icmap_item *)old_value;
	struct icmap_notify_value new_val;
	struct icmap_notify_value old_val;

	if (value == NULL && old_value == NULL) {
		return ;
	}

	if (new_item != NULL) {
		new_val.type = new_item->type;
		new_val.len = new_item->value_len;
		new_val.data = new_item->value;
	} else {
		memset(&new_val, 0, sizeof(new_val));
	}

	if (old_item != NULL) {
		old_val.type = old_item->type;
		old_val.len = old_item->value_len;
		old_val.data = old_item->value;
	} else {
		memset(&old_val, 0, sizeof(old_val));
	}

	icmap_track->notify_fn(icmap_qbtt_to_tt(event),
			key,
			new_val,
			old_val,
			icmap_track->user_data);
}

cs_error_t icmap_track_add(
	const char *key_name,
	int32_t track_type,
	icmap_notify_fn_t notify_fn,
	void *user_data,
	icmap_track_t *icmap_track)
{
	int32_t err;

	if (notify_fn == NULL || icmap_track == NULL) {
		return (CS_ERR_INVALID_PARAM);
	}

	if ((track_type & ~(ICMAP_TRACK_ADD | ICMAP_TRACK_DELETE | ICMAP_TRACK_MODIFY | ICMAP_TRACK_PREFIX)) != 0) {
		return (CS_ERR_INVALID_PARAM);
	}

	*icmap_track = malloc(sizeof(**icmap_track));
	if (*icmap_track == NULL) {
		return (CS_ERR_NO_MEMORY);
	}
	memset(*icmap_track, 0, sizeof(**icmap_track));

	if (key_name != NULL) {
		(*icmap_track)->key_name = strdup(key_name);
	};

	(*icmap_track)->track_type = track_type;
	(*icmap_track)->notify_fn = notify_fn;
	(*icmap_track)->user_data = user_data;

	if ((err = qb_map_notify_add(icmap_map, (*icmap_track)->key_name, icmap_notify_fn,
					icmap_tt_to_qbtt(track_type), *icmap_track)) != 0) {
		free((*icmap_track)->key_name);
		free(*icmap_track);

		return (qb_to_cs_error(err));
	}

	return (CS_OK);
}

cs_error_t icmap_track_delete(icmap_track_t icmap_track)
{
	int32_t err;

	if ((err = qb_map_notify_del_2(icmap_map, icmap_track->key_name,
				icmap_notify_fn, icmap_tt_to_qbtt(icmap_track->track_type), icmap_track)) != 0) {
		return (qb_to_cs_error(err));
	}

	free(icmap_track->key_name);
	free(icmap_track);

	return (CS_OK);
}

void *icmap_track_get_user_data(icmap_track_t icmap_track)
{
	return (icmap_track->user_data);
}

cs_error_t icmap_set_ro_access(const char *key_name, int prefix, int ro_access)
{
	struct list_head *iter;
	struct icmap_ro_access_item *icmap_ro_ai;

	for (iter = icmap_ro_access_item_list_head.next; iter != &icmap_ro_access_item_list_head; iter = iter->next) {
		icmap_ro_ai = list_entry(iter, struct icmap_ro_access_item, list);

		if (icmap_ro_ai->prefix == prefix && strcmp(key_name, icmap_ro_ai->key_name) == 0) {
			/*
			 * We found item
			 */
			if (ro_access) {
				return (CS_ERR_EXIST);
			} else {
				list_del(&icmap_ro_ai->list);
				free(icmap_ro_ai);

				return (CS_OK);
			}
		}
	}

	if (!ro_access) {
		return (CS_ERR_NOT_EXIST);
	}

	icmap_ro_ai = malloc(sizeof(*icmap_ro_ai));
	if (icmap_ro_ai == NULL) {
		return (CS_ERR_NO_MEMORY);
	}

	memset(icmap_ro_ai, 0, sizeof(*icmap_ro_ai));
	icmap_ro_ai->key_name = strdup(key_name);
	if (icmap_ro_ai->key_name == NULL) {
		free(icmap_ro_ai);
		return (CS_ERR_NO_MEMORY);
	}

	icmap_ro_ai->prefix = prefix;
	list_init(&icmap_ro_ai->list);
	list_add (&icmap_ro_ai->list, &icmap_ro_access_item_list_head);

	return (CS_OK);
}

int icmap_is_key_ro(const char *key_name)
{
	struct list_head *iter;
	struct icmap_ro_access_item *icmap_ro_ai;

	for (iter = icmap_ro_access_item_list_head.next; iter != &icmap_ro_access_item_list_head; iter = iter->next) {
		icmap_ro_ai = list_entry(iter, struct icmap_ro_access_item, list);

		if (icmap_ro_ai->prefix) {
			if (strlen(icmap_ro_ai->key_name) > strlen(key_name))
				continue;

			if (strncmp(icmap_ro_ai->key_name, key_name, strlen(icmap_ro_ai->key_name)) == 0) {
				return (CS_TRUE);
			}
		} else {
			if (strcmp(icmap_ro_ai->key_name, key_name) == 0) {
				return (CS_TRUE);
			}
		}
	}

	return (CS_FALSE);

}
