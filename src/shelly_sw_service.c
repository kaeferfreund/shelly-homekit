/*
 * Copyright (c) 2020 Deomid "rojer" Ryabkov
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 *limitations under the License.
 */

#include "shelly_sw_service.h"

#include <math.h>

#include "mgos.h"
#ifdef MGOS_HAVE_ADE7953
#include "mgos_ade7953.h"
#endif

#define IID_BASE 0x100
#define IID_STEP 4

enum shelly_sw_in_mode {
  SHELLY_SW_IN_MODE_MOMENTARY = 0,
  SHELLY_SW_IN_MODE_TOGGLE = 1,
  SHELLY_SW_IN_MODE_EDGE = 2,
  SHELLY_SW_IN_MODE_DETACHED = 3,
};

struct shelly_sw_service_ctx {
  const struct mgos_config_sw *cfg;
  HAPAccessoryServerRef *hap_server;
  const HAPAccessory *hap_accessory;
  const HAPService *hap_service;
  struct shelly_sw_info info;
  bool pb_state;
  int change_cnt;         // State change counter for reset.
  double last_change_ts;  // Timestamp of last change (uptime).
#ifdef MGOS_HAVE_ADE7953
  struct mgos_ade7953 *ade7953;
  int ade7953_channel;
#endif
};

static struct shelly_sw_service_ctx s_ctx[NUM_SWITCHES];

#ifdef SHELLY_HAVE_PM
static void shelly_sw_read_power(void *arg);
#endif

static void do_reset(void *arg) {
  struct shelly_sw_service_ctx *ctx = arg;
  mgos_gpio_blink(ctx->cfg->out_gpio, 0, 0);
  LOG(LL_INFO, ("Performing reset"));
#ifdef MGOS_SYS_CONFIG_HAVE_WIFI
  mgos_sys_config_set_wifi_sta_enable(false);
  mgos_sys_config_set_wifi_ap_enable(true);
  mgos_sys_config_save(&mgos_sys_config, false, NULL);
  mgos_wifi_setup((struct mgos_config_wifi *) mgos_sys_config_get_wifi());
#endif
}

static void shelly_sw_set_state_ctx(struct shelly_sw_service_ctx *ctx,
                                    bool new_state, const char *source) {
  const struct mgos_config_sw *cfg = ctx->cfg;
  if (new_state == ctx->info.state) return;

  int out_value = (new_state ? cfg->out_on_value : !cfg->out_on_value);
  mgos_gpio_write(cfg->out_gpio, out_value);
  LOG(LL_INFO, ("%s: %d -> %d (%s) %d", cfg->name, ctx->info.state, new_state,
                source, out_value));
  ctx->info.state = new_state;
  if (ctx->hap_server != NULL) {
    HAPAccessoryServerRaiseEvent(ctx->hap_server,
                                 ctx->hap_service->characteristics[1],
                                 ctx->hap_service, ctx->hap_accessory);
  }
  if (cfg->persist_state) {
    ((struct mgos_config_sw *) cfg)->state = new_state;
    mgos_sys_config_save(&mgos_sys_config, false /* try_once */,
                         NULL /* msg */);
  }


  
  double now = mgos_uptime();
  if (now < 60) {
    if (now - ctx->last_change_ts > 10) {
      ctx->change_cnt = 0;
    }
    ctx->change_cnt++;
    ctx->last_change_ts = now;
    if (ctx->change_cnt >= 100) {
      LOG(LL_INFO, ("Reset sequence detected"));
      ctx->change_cnt = 0;
      mgos_gpio_blink(cfg->out_gpio, 100, 100);
      mgos_set_timer(600, 0, do_reset, ctx);
    }
  }
}

bool shelly_sw_set_state(int id, bool new_state, const char *source) {
  if (id < 0 || id >= NUM_SWITCHES) return false;
  struct shelly_sw_service_ctx *ctx = &s_ctx[id];
  if (ctx == NULL) return false;
  shelly_sw_set_state_ctx(ctx, new_state, source);
  return true;
}

bool shelly_sw_get_info(int id, struct shelly_sw_info *info) {
  if (id < 0 || id >= NUM_SWITCHES) return false;
  struct shelly_sw_service_ctx *ctx = &s_ctx[id];
  if (ctx == NULL) return false;
  *info = ctx->info;
  return true;
}

static struct shelly_sw_service_ctx *find_ctx(const HAPService *svc) {
  for (size_t i = 0; i < ARRAY_SIZE(s_ctx); i++) {
    if (s_ctx[i].hap_service == svc) return &s_ctx[i];
  }
  return NULL;
}


HAPError on_write_shutter_target(
    HAPAccessoryServerRef *server,
    const HAPUInt8CharacteristicWriteRequest *request, 
    uint8_t value,
    void* context) {

  struct shelly_sw_service_ctx *ctx = find_ctx(request->service);
  ctx->hap_server = server;
  ctx->hap_accessory = request->accessory;

  //shelly_sw_set_state_ctx(ctx, value, "HAP");

  value = 2;


  (void) context;
  return kHAPError_None;
}

static void shelly_sw_in_cb(int pin, void *arg) {
  struct shelly_sw_service_ctx *ctx = arg;
  bool in_state = mgos_gpio_read(pin);
  switch ((enum shelly_sw_in_mode) ctx->cfg->in_mode) {
    case SHELLY_SW_IN_MODE_MOMENTARY:
      if (in_state) {  // Only on 0 -> 1 transitions.
        shelly_sw_set_state_ctx(ctx, !ctx->info.state, "button");
      }
      break;
    case SHELLY_SW_IN_MODE_TOGGLE:
      shelly_sw_set_state_ctx(ctx, in_state, "switch");
      break;
    case SHELLY_SW_IN_MODE_EDGE:
      shelly_sw_set_state_ctx(ctx, !ctx->info.state, "button");
      break;
    case SHELLY_SW_IN_MODE_DETACHED:
      // Nothing to do    
      break;
  }
  (void) pin;
}

#ifdef SHELLY_HAVE_PM
static void shelly_sw_read_power(void *arg) {
  struct shelly_sw_service_ctx *ctx = arg;
#ifdef MGOS_HAVE_ADE7953
  float apa = 0, aea = 0;
  if (mgos_ade7953_get_apower(ctx->ade7953, ctx->ade7953_channel, &apa)) {
    if (fabs(apa) < 0.5) apa = 0;  // Suppress noise.
    ctx->info.apower = fabs(apa);
  }
  if (mgos_ade7953_get_aenergy(ctx->ade7953, ctx->ade7953_channel,
                               true /* reset */, &aea)) {
    ctx->info.aenergy += fabs(aea);
  }
#endif
}
#endif

static const HAPUInt8Characteristic *shutter_current_position(uint16_t iid) {
  HAPUInt8Characteristic *c = calloc(1, sizeof(*c));
  if (c == NULL) return NULL;
  *c = (const HAPUInt8Characteristic){
      .format = kHAPCharacteristicFormat_UInt8,
      .iid = iid,
      .characteristicType = &kHAPCharacteristicType_CurrentPosition,
      .debugDescription = kHAPCharacteristicDebugDescription_CurrentPosition,
      .manufacturerDescription = NULL,
      .properties =
          {
              .readable = true,
              .writable = false,
              .supportsEventNotification = false,
              .hidden = false,
              .requiresTimedWrite = false,
              .supportsAuthorizationData = false,
              .ip =
                  {
                      .controlPoint = false,
                      .supportsWriteResponse = false,
                  },
              .ble =
                  {
                      .supportsBroadcastNotification = false,
                      .supportsDisconnectedNotification = false,
                      .readableWithoutSecurity = false,
                      .writableWithoutSecurity = false,
                  },
          },
      .units = kHAPCharacteristicUnits_Percentage,
      .constraints = { .minimumValue = 0,
                     .maximumValue = 100,
                     .stepValue = 1,
                     .validValues = NULL,
                     .validValuesRanges = NULL },
      .callbacks = {.handleRead = 0, .handleWrite = NULL},
  };
  return c;
};

static const HAPUInt8Characteristic *shutter_target_position(uint16_t iid) {
  HAPUInt8Characteristic *c = calloc(1, sizeof(*c));
  if (c == NULL) return NULL;
  *c = (const HAPUInt8Characteristic){
      .format = kHAPCharacteristicFormat_UInt8,
      .iid = iid,
      .characteristicType = &kHAPCharacteristicType_TargetPosition,
      .debugDescription = kHAPCharacteristicDebugDescription_TargetPosition,
      .manufacturerDescription = NULL,
      .properties =
          {
              .readable = true,
              .writable = true,
              .supportsEventNotification = true,
              .hidden = false,
              .requiresTimedWrite = false,
              .supportsAuthorizationData = false,
              .ip =
                  {
                      .controlPoint = false,
                      .supportsWriteResponse = false,
                  },
              .ble =
                  {
                      .supportsBroadcastNotification = true,
                      .supportsDisconnectedNotification = true,
                      .readableWithoutSecurity = false,
                      .writableWithoutSecurity = false,
                  },
          },
      .units = kHAPCharacteristicUnits_Percentage,
      .constraints = { .minimumValue = 0,
                     .maximumValue = 100,
                     .stepValue = 1,
                     .validValues = NULL,
                     .validValuesRanges = NULL },
      .callbacks = {
                      .handleRead = 0, 
                      .handleWrite = on_write_shutter_target},
  };
  return c;
};

static const HAPUInt8Characteristic *shutter_position_state(uint16_t iid) {
  HAPUInt8Characteristic *c = calloc(1, sizeof(*c));
  if (c == NULL) return NULL;
  *c = (const HAPUInt8Characteristic){
      .format = kHAPCharacteristicFormat_UInt8,
      .iid = iid,
      .characteristicType = &kHAPCharacteristicType_PositionState,
      .debugDescription = kHAPCharacteristicDebugDescription_PositionState,
      .manufacturerDescription = NULL,
      .properties =
          {
              .readable = true,
              .writable = false,
              .supportsEventNotification = false,
              .hidden = false,
              .requiresTimedWrite = false,
              .supportsAuthorizationData = false,
              .ip =
                  {
                      .controlPoint = false,
                      .supportsWriteResponse = false,
                  },
              .ble =
                  {
                      .supportsBroadcastNotification = false,
                      .supportsDisconnectedNotification = false,
                      .readableWithoutSecurity = false,
                      .writableWithoutSecurity = false,
                  },
          },
      .callbacks = {  .handleRead = 0, 
                      .handleWrite = NULL},
  };
  return c;
};

static const HAPBoolCharacteristic *shutter_hold_position(uint16_t iid) {
  HAPBoolCharacteristic *c = calloc(1, sizeof(*c));
  if (c == NULL) return NULL;
  *c = (const HAPBoolCharacteristic){
      .format = kHAPCharacteristicFormat_Bool,
      .iid = iid,
      .characteristicType = &kHAPCharacteristicType_HoldPosition,
      .debugDescription = kHAPCharacteristicDebugDescription_HoldPosition,
      .manufacturerDescription = NULL,
      .properties = 
          {
              .readable = true,
              .writable = false,
              .supportsEventNotification = false,
              .hidden = false,
              .requiresTimedWrite = false,
              .supportsAuthorizationData = false,
              .ip =
                  {
                      .controlPoint = false,
                      .supportsWriteResponse = false,
                  },
              .ble =
                  {
                      .supportsBroadcastNotification = false,
                      .supportsDisconnectedNotification = false,
                      .readableWithoutSecurity = false,
                      .writableWithoutSecurity = false,
                  },
          },
      .callbacks = {
        .handleRead = 0, 
        .handleWrite = NULL},
  };
  return c;
};

static const HAPBoolCharacteristic *shutter_obstruction_detected(uint16_t iid) {
  HAPBoolCharacteristic *c = calloc(1, sizeof(*c));
  if (c == NULL) return NULL;
  *c = (const HAPBoolCharacteristic){
      .format = kHAPCharacteristicFormat_Bool,
      .iid = iid,
      .characteristicType = &kHAPCharacteristicType_ObstructionDetected,
      .debugDescription = kHAPCharacteristicDebugDescription_ObstructionDetected,
      .manufacturerDescription = NULL,
      .properties = 
          {
              .readable = true,
              .writable = false,
              .supportsEventNotification = false,
              .hidden = false,
              .requiresTimedWrite = false,
              .supportsAuthorizationData = false,
              .ip =
                  {
                      .controlPoint = false,
                      .supportsWriteResponse = false,
                  },
              .ble =
                  {
                      .supportsBroadcastNotification = false,
                      .supportsDisconnectedNotification = false,
                      .readableWithoutSecurity = false,
                      .writableWithoutSecurity = false,
                  },
          },
      .callbacks = {.handleRead = 0, .handleWrite = NULL},
  };
  return c;
};

HAPService *shelly_sw_service_create(
#ifdef MGOS_HAVE_ADE7953
    struct mgos_ade7953 *ade7953, int ade7953_channel,
#endif
    const struct mgos_config_sw *cfg) {
  if (!cfg->enable) {
    LOG(LL_INFO, ("'%s' is disabled", cfg->name));
    mgos_gpio_setup_output(cfg->out_gpio, !cfg->out_on_value);
    return NULL;
  }
  if (cfg->id >= NUM_SWITCHES) {
    LOG(LL_ERROR, ("Switch ID too big!"));
    return NULL;
  }
  HAPService *svc = calloc(1, sizeof(*svc));
  if (svc == NULL) return NULL;
  const HAPCharacteristic **chars = calloc(5, sizeof(*chars));
  if (chars == NULL) return NULL;
  svc->iid = IID_BASE + (IID_STEP * cfg->id) + 0;
  svc->serviceType = &kHAPServiceType_Window;
  svc->debugDescription = kHAPServiceDebugDescription_Window;
  svc->name = cfg->name;
  svc->properties.primaryService = true;

  chars[0] = shutter_current_position(IID_BASE + (IID_STEP * cfg->id) + 1);
  chars[1] = shutter_target_position(IID_BASE + (IID_STEP * cfg->id) + 2);
  chars[2] = shutter_position_state(IID_BASE + (IID_STEP * cfg->id) + 3);
  chars[3] = shutter_hold_position(IID_BASE + (IID_STEP * cfg->id) + 4);
  chars[4] = shutter_obstruction_detected(IID_BASE + (IID_STEP * cfg->id) + 5);
  chars[5] = NULL;

  svc->characteristics = chars;


  struct shelly_sw_service_ctx *ctx = &s_ctx[cfg->id];
  ctx->cfg = cfg;
  ctx->hap_service = svc;
  if (cfg->persist_state) {
    ctx->info.state = cfg->state;
  } else {
    ctx->info.state = 0;
  }
  LOG(LL_INFO, ("Exporting '%s' (GPIO out: %d, in: %d, state: %d)", cfg->name,
                cfg->out_gpio, cfg->in_gpio, ctx->info.state));
  mgos_gpio_setup_output(cfg->out_gpio, (ctx->info.state ? cfg->out_on_value
                                                         : !cfg->out_on_value));
  mgos_gpio_set_button_handler(cfg->in_gpio, MGOS_GPIO_PULL_NONE,
                               MGOS_GPIO_INT_EDGE_ANY, 20, shelly_sw_in_cb,
                               ctx);
  if (ctx->cfg->in_mode == SHELLY_SW_IN_MODE_TOGGLE) {
    shelly_sw_in_cb(cfg->in_gpio, ctx);
  }
#ifdef SHELLY_HAVE_PM
#ifdef MGOS_HAVE_ADE7953
  ctx->ade7953 = ade7953;
  ctx->ade7953_channel = ade7953_channel;
#endif
  mgos_set_timer(1000, MGOS_TIMER_REPEAT, shelly_sw_read_power, ctx);
#endif
  return svc;
}
