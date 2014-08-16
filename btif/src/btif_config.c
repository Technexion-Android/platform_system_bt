/******************************************************************************
 *
 *  Copyright (C) 2014 Google, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "bt_btif_config"

#include <assert.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <utils/Log.h>

#include "alarm.h"
#include "btif_config.h"
#include "btif_config_transcode.h"
#include "btif_util.h"
#include "config.h"
#include "osi.h"

#include "bd.h"

static const char *CONFIG_FILE_PATH = "/data/misc/bluedroid/bt_config.conf";
static const char *LEGACY_CONFIG_FILE_PATH = "/data/misc/bluedroid/bt_config.xml";
static const period_ms_t CONFIG_SETTLE_PERIOD_MS = 3000;

static void timer_config_save(void *data);

// TODO(zachoverflow): Move these two functions out, because they are too specific for this file
// {grumpy-cat/no, monty-python/you-make-me-sad}
bool btif_get_device_type(const BD_ADDR bd_addr, int *p_device_type)
{
    if (p_device_type == NULL)
        return FALSE;

    bt_bdaddr_t bda;
    bdcpy(bda.address, bd_addr);

    char bd_addr_str[18] = {0};
    bd2str(&bda, &bd_addr_str);

    if (!btif_config_get_int("Remote", bd_addr_str, "DevType", p_device_type))
        return FALSE;

    ALOGD("%s: Device [%s] type %d", __FUNCTION__, bd_addr_str, *p_device_type);
    return TRUE;
}

bool btif_get_address_type(const BD_ADDR bd_addr, int *p_addr_type)
{
    if (p_addr_type == NULL)
        return FALSE;

    bt_bdaddr_t bda;
    bdcpy(bda.address, bd_addr);

    char bd_addr_str[18] = {0};
    bd2str(&bda, &bd_addr_str);

    if (!btif_config_get_int("Remote", bd_addr_str, "AddrType", p_addr_type))
        return FALSE;

    ALOGD("%s: Device [%s] address type %d", __FUNCTION__, bd_addr_str, *p_addr_type);
    return TRUE;
}

static pthread_mutex_t lock;  // protects operations on |config|.
static config_t *config;
static alarm_t *alarm_timer;

bool btif_config_init(void) {
  pthread_mutex_init(&lock, NULL);
  config = config_new(CONFIG_FILE_PATH);
  if (!config) {
    ALOGW("%s unable to load config file; attempting to transcode legacy file.", __func__);
    config = btif_config_transcode(LEGACY_CONFIG_FILE_PATH);
    if (!config) {
      ALOGW("%s unable to transcode legacy file, starting unconfigured.", __func__);
      config = config_new_empty();
      if (!config) {
        ALOGE("%s unable to allocate a config object.", __func__);
        goto error;
      }
    }

    if (config_save(config, CONFIG_FILE_PATH))
      unlink(LEGACY_CONFIG_FILE_PATH);
  }

  // TODO(sharvil): use a non-wake alarm for this once we have
  // API support for it. Threre's no need to wake the system to
  // write back to disk.
  alarm_timer = alarm_new();
  if (!alarm_timer) {
    ALOGE("%s unable to create alarm.", __func__);
    goto error;
  }

  return true;

error:;
  alarm_free(alarm_timer);
  config_free(config);
  pthread_mutex_destroy(&lock);
  alarm_timer = NULL;
  config = NULL;
  return false;
}

void btif_config_cleanup(void) {
  btif_config_flush();

  alarm_free(alarm_timer);
  config_free(config);
  pthread_mutex_destroy(&lock);
  alarm_timer = NULL;
  config = NULL;
}

bool btif_config_has_key(UNUSED_ATTR const char *section, const char *key) {
  assert(config != NULL);
  assert(key != NULL);

  pthread_mutex_lock(&lock);
  bool ret = config_has_section(config, key);
  pthread_mutex_unlock(&lock);

  return ret;
}

bool btif_config_exist(UNUSED_ATTR const char *section, const char *key, const char *name) {
  assert(config != NULL);
  assert(key != NULL);
  assert(name != NULL);

  pthread_mutex_lock(&lock);
  bool ret = config_has_key(config, key, name);
  pthread_mutex_unlock(&lock);

  return ret;
}

bool btif_config_get_int(UNUSED_ATTR const char *section, const char *key, const char *name, int *value) {
  assert(config != NULL);
  assert(key != NULL);
  assert(name != NULL);
  assert(value != NULL);

  pthread_mutex_lock(&lock);
  bool ret = config_has_key(config, key, name);
  if (ret)
    *value = config_get_int(config, key, name, *value);
  pthread_mutex_unlock(&lock);

  return ret;
}

bool btif_config_set_int(UNUSED_ATTR const char *section, const char *key, const char *name, int value) {
  assert(config != NULL);
  assert(key != NULL);
  assert(name != NULL);

  pthread_mutex_lock(&lock);
  config_set_int(config, key, name, value);
  pthread_mutex_unlock(&lock);

  return true;
}

bool btif_config_get_str(UNUSED_ATTR const char *section, const char *key, const char *name, char *value, int *length) {
  assert(config != NULL);
  assert(key != NULL);
  assert(name != NULL);
  assert(value != NULL);
  assert(length != NULL);

  pthread_mutex_lock(&lock);
  const char *stored_value = config_get_string(config, key, name, NULL);
  pthread_mutex_unlock(&lock);

  if (!stored_value)
    return false;

  strlcpy(value, stored_value, *length);
  *length = strlen(value);

  return true;
}

bool btif_config_set_str(UNUSED_ATTR const char *section, const char *key, const char *name, const char *value) {
  assert(config != NULL);
  assert(key != NULL);
  assert(name != NULL);
  assert(value != NULL);

  pthread_mutex_lock(&lock);
  config_set_string(config, key, name, value);
  pthread_mutex_unlock(&lock);

  return true;
}

bool btif_config_get_bin(UNUSED_ATTR const char *section, const char *key, const char *name, uint8_t *value, size_t *length) {
  assert(config != NULL);
  assert(key != NULL);
  assert(name != NULL);
  assert(value != NULL);
  assert(length != NULL);

  pthread_mutex_lock(&lock);
  const char *value_str = config_get_string(config, key, name, NULL);
  pthread_mutex_unlock(&lock);

  if (!value_str)
    return false;

  size_t value_len = strlen(value_str);
  if ((value_len % 2) != 0 || *length < (value_len / 2))
    return false;

  for (size_t i = 0; i < value_len; ++i)
    if (!isxdigit(value_str[i]))
      return false;

  for (*length = 0; *value_str; value_str += 2, *length += 1)
    sscanf(value_str, "%02hhx", &value[*length]);

  return true;
}

size_t btif_config_get_bin_length(UNUSED_ATTR const char *section, const char *key, const char *name) {
  assert(config != NULL);
  assert(key != NULL);
  assert(name != NULL);

  pthread_mutex_lock(&lock);
  const char *value_str = config_get_string(config, key, name, NULL);
  pthread_mutex_unlock(&lock);

  if (!value_str)
    return 0;

  size_t value_len = strlen(value_str);
  return ((value_len % 2) != 0) ? 0 : (value_len / 2);
}

bool btif_config_set_bin(UNUSED_ATTR const char *section, const char *key, const char *name, const uint8_t *value, size_t length) {
  static const char *lookup = "0123456789abcdef";

  assert(config != NULL);
  assert(key != NULL);
  assert(name != NULL);
  assert(value != NULL);

  char *str = (char *)calloc(length * 2 + 1, 1);
  if (!str)
    return false;

  for (size_t i = 0; i < length; ++i) {
    str[(i * 2) + 0] = lookup[(value[i] >> 0) & 0x0F];
    str[(i * 2) + 1] = lookup[(value[i] >> 4) & 0x0F];
  }

  pthread_mutex_lock(&lock);
  config_set_string(config, key, name, str);
  pthread_mutex_unlock(&lock);

  free(str);
  return true;
}

const btif_config_section_iter_t *btif_config_section_begin(void) {
  assert(config != NULL);
  return (const btif_config_section_iter_t *)config_section_begin(config);
}

const btif_config_section_iter_t *btif_config_section_end(void) {
  assert(config != NULL);
  return (const btif_config_section_iter_t *)config_section_end(config);
}

const btif_config_section_iter_t *btif_config_section_next(const btif_config_section_iter_t *section) {
  assert(config != NULL);
  assert(section != NULL);
  return (const btif_config_section_iter_t *)config_section_next((const config_section_node_t *)section);
}

const char *btif_config_section_name(const btif_config_section_iter_t *section) {
  assert(config != NULL);
  assert(section != NULL);
  return config_section_name((const config_section_node_t *)section);
}

bool btif_config_remove(UNUSED_ATTR const char *section, const char *key, const char *name) {
  assert(config != NULL);
  assert(key != NULL);
  assert(name != NULL);

  pthread_mutex_lock(&lock);
  bool ret = config_remove_key(config, key, name);
  pthread_mutex_unlock(&lock);

  return ret;
}

void btif_config_save(void) {
  assert(alarm_timer != NULL);
  assert(config != NULL);

  alarm_set(alarm_timer, CONFIG_SETTLE_PERIOD_MS, timer_config_save, NULL);
}

void btif_config_flush(void) {
  assert(config != NULL);
  assert(alarm_timer != NULL);

  alarm_cancel(alarm_timer);

  pthread_mutex_lock(&lock);
  config_save(config, CONFIG_FILE_PATH);
  pthread_mutex_unlock(&lock);
}

static void timer_config_save(UNUSED_ATTR void *data) {
  assert(config != NULL);
  assert(alarm_timer != NULL);

  // Garbage collection process: the config file accumulates
  // cached information about remote devices during regular
  // inquiry scans. We remove some of these junk entries
  // so the file doesn't grow indefinitely. We have to take care
  // to make sure we don't remove information about bonded
  // devices (hence the check for link keys).
  static const size_t CACHE_MAX = 256;
  const char *keys[CACHE_MAX];
  size_t num_keys = 0;
  size_t total_candidates = 0;

  pthread_mutex_lock(&lock);
  for (const config_section_node_t *snode = config_section_begin(config); snode != config_section_end(config); snode = config_section_next(snode)) {
    const char *section = config_section_name(snode);
    if (!str_is_bdaddr(section))
      continue;

    if (config_has_key(config, section, "LinkKey") ||
        config_has_key(config, section, "LE_KEY_PENC") ||
        config_has_key(config, section, "LE_KEY_PID") ||
        config_has_key(config, section, "LE_KEY_PCSRK") ||
        config_has_key(config, section, "LE_KEY_LENC") ||
        config_has_key(config, section, "LE_KEY_LCSRK"))
      continue;

    if (num_keys < CACHE_MAX)
      keys[num_keys++] = section;

    ++total_candidates;
  }

  if (total_candidates > CACHE_MAX * 2)
    while (num_keys > 0)
      config_remove_section(config, keys[--num_keys]);

  config_save(config, CONFIG_FILE_PATH);
  pthread_mutex_unlock(&lock);
}
