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

struct shelly_sw_service_ctx {
  const struct mgos_config_sw *cfg;
  HAPAccessoryServerRef *hap_server;
  const HAPAccessory *hap_accessory;
  const HAPService *hap_service;
  struct shelly_sw_info info;
  bool pb_state;
  int change_cnt;         // State change counter for reset.
  double last_change_ts;  // Timestamp of last change (uptime).
  mgos_timer_id auto_off_timer_id;
#ifdef MGOS_HAVE_ADE7953
  struct mgos_ade7953 *ade7953;
  int ade7953_channel;
#endif
};

static struct shelly_sw_service_ctx s_ctx[NUM_SWITCHES];

static void do_auto_off(void *arg);

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

static void handle_auto_off(struct shelly_sw_service_ctx *ctx,
                            const char *source, bool new_state) {
  if (ctx->auto_off_timer_id != MGOS_INVALID_TIMER_ID) {
    // Cancel timer if state changes so that only the last timer is triggered if
    // state changes multiple times
    mgos_clear_timer(ctx->auto_off_timer_id);
    ctx->auto_off_timer_id = MGOS_INVALID_TIMER_ID;
    LOG(LL_INFO, ("%d: Cleared auto-off timer", ctx->cfg->id));
  }

  const struct mgos_config_sw *cfg = ctx->cfg;

  if (!cfg->auto_off) return;

  if (strcmp(source, "auto_off") == 0) return;

  if (!new_state) return;

  ctx->auto_off_timer_id =
      mgos_set_timer(cfg->auto_off_delay * 1000, 0, do_auto_off, ctx);
  LOG(LL_INFO, ("%d: Set auto-off timer for %d", cfg->id, cfg->auto_off_delay));
}

// 0 stop, 1 up, 2 down
static void set_gpio_shutter(int direction){
  
  if (direction == 0) {
    mgos_gpio_setup_output(s_ctx[0].cfg->out_gpio, 0);
    mgos_gpio_setup_output(s_ctx[1].cfg->out_gpio, 0);
    s_ctx[0].info.state = 0;
    s_ctx[1].info.state = 0;
  }
  if (direction == 1) {
    mgos_gpio_setup_output(s_ctx[0].cfg->out_gpio, 0);
    mgos_gpio_setup_output(s_ctx[1].cfg->out_gpio, 1);
    s_ctx[0].info.state = 0;
    s_ctx[1].info.state = 1;
  }
  if (direction == 2) {
    mgos_gpio_setup_output(s_ctx[0].cfg->out_gpio, 1);
    mgos_gpio_setup_output(s_ctx[1].cfg->out_gpio, 0);
    s_ctx[0].info.state = 1;
    s_ctx[1].info.state = 0;
  }
  return;
}

static void shelly_sw_set_state_ctx(struct shelly_sw_service_ctx *ctx,
                                    bool new_state, const char *source) {
  const struct mgos_config_sw *cfg = ctx->cfg;
  int out_value = (new_state ? cfg->out_on_value : !cfg->out_on_value);

  // set gpio here
  // if(SHELLY_SHUTTER_MODE == 1){
  if (new_state == ctx->info.state) return;
    if (!new_state){
      set_gpio_shutter(0);
    }
    if (new_state){
      if (ctx->cfg->out_gpio == s_ctx[0].cfg->out_gpio){
        set_gpio_shutter(1);
      }
      if (ctx->cfg->out_gpio == s_ctx[1].cfg->out_gpio){
        set_gpio_shutter(2);
      }
    }
  // }
  
  return;
  LOG(LL_INFO, ("%s: %d -> %d (%s) %d", cfg->name, ctx->info.state, new_state,
                source, out_value));
  ctx->info.state = new_state;
  if (ctx->hap_server != NULL) {
    HAPAccessoryServerRaiseEvent(ctx->hap_server,
                                 ctx->hap_service->characteristics[1],
                                 ctx->hap_service, ctx->hap_accessory);
  }
  if (cfg->state != new_state) {
    ((struct mgos_config_sw *) cfg)->state = new_state;
    if (cfg->initial_state == SHELLY_SW_INITIAL_STATE_LAST) {
      mgos_sys_config_save(&mgos_sys_config, false /* try_once */,
                           NULL /* msg */);
    }
  }

  handle_auto_off(ctx, source, new_state);
}

static void do_auto_off(void *arg) {
  struct shelly_sw_service_ctx *ctx = arg;
  const struct mgos_config_sw *cfg = ctx->cfg;
  ctx->auto_off_timer_id = MGOS_INVALID_TIMER_ID;
  LOG(LL_INFO, ("%d: Auto-off timer fired", cfg->id));
  if (cfg->auto_off) {
    // Don't set state if auto off has been disabled during timer run
    shelly_sw_set_state_ctx(ctx, false, "auto_off");
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

HAPError on_write_shutter_target(
     HAPAccessoryServerRef *server,
     const HAPUInt8CharacteristicWriteRequest *request, 
     uint8_t value,
     void* context){
   struct shelly_sw_service_ctx *ctx = find_ctx(request->service);
   ctx->hap_server = server;
   ctx->hap_accessory = request->accessory;

   if (value == 2) {
    return kHAPError_None;
   }
   // shelly_sw_set_state_ctx(ctx, value, "HAP");
   // shelly_sw_in_cb(int(value), ctx);
   (void) context;
   return kHAPError_None;
 }

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

HAPError shelly_sw_handle_on_read(
    HAPAccessoryServerRef *server,
    const HAPBoolCharacteristicReadRequest *request, bool *value,
    void *context) {
  struct shelly_sw_service_ctx *ctx = find_ctx(request->service);
  const struct mgos_config_sw *cfg = ctx->cfg;
  *value = ctx->info.state;
  LOG(LL_INFO, ("%s: READ -> %d", cfg->name, ctx->info.state));
  ctx->hap_server = server;
  ctx->hap_accessory = request->accessory;
  (void) context;
  return kHAPError_None;
}

HAPError shelly_sw_handle_on_write(
    HAPAccessoryServerRef *server,
    const HAPBoolCharacteristicWriteRequest *request, bool value,
    void *context) {
  struct shelly_sw_service_ctx *ctx = find_ctx(request->service);
  ctx->hap_server = server;
  ctx->hap_accessory = request->accessory;
  shelly_sw_set_state_ctx(ctx, value, "HAP");
  (void) context;
  return kHAPError_None;
}

static void shelly_sw_in_cb(int pin, void *arg) {
  struct shelly_sw_service_ctx *ctx = arg;
  bool in_state = mgos_gpio_read(pin);


  double now = mgos_uptime();
  if (now < 60) {
    if (now - ctx->last_change_ts > 10) {
      ctx->change_cnt = 0;
    }
    ctx->change_cnt++;
    ctx->last_change_ts = now;
    if (ctx->change_cnt >= 10) {
      LOG(LL_INFO, ("Reset sequence detected"));
      ctx->change_cnt = 0;
      mgos_gpio_blink(ctx->cfg->out_gpio, 100, 100);
      mgos_set_timer(600, 0, do_reset, ctx);
      return;
    }
  }

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
  svc->serviceType = &kHAPServiceType_Switch;
  svc->debugDescription = kHAPServiceDebugDescription_Switch;
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
  ctx->auto_off_timer_id = MGOS_INVALID_TIMER_ID;
  LOG(LL_INFO, ("Exporting '%s' (GPIO out: %d, in: %d, state: %d)", cfg->name,
                cfg->out_gpio, cfg->in_gpio, ctx->info.state));

  switch ((enum shelly_sw_initial_state) cfg->initial_state) {
    case SHELLY_SW_INITIAL_STATE_OFF:
      shelly_sw_set_state_ctx(ctx, false, "init");
      break;
    case SHELLY_SW_INITIAL_STATE_ON:
      shelly_sw_set_state_ctx(ctx, true, "init");
      break;
    case SHELLY_SW_INITIAL_STATE_LAST:
      shelly_sw_set_state_ctx(ctx, cfg->state, "init");
      break;
    case SHELLY_SW_INITIAL_STATE_INPUT:
      if (cfg->in_mode == SHELLY_SW_IN_MODE_TOGGLE) {
        shelly_sw_in_cb(cfg->in_gpio, ctx);
      }
  }
  mgos_gpio_set_button_handler(cfg->in_gpio, MGOS_GPIO_PULL_NONE,
                               MGOS_GPIO_INT_EDGE_ANY, 20, shelly_sw_in_cb,
                               ctx);
#ifdef SHELLY_HAVE_PM
#ifdef MGOS_HAVE_ADE7953
  ctx->ade7953 = ade7953;
  ctx->ade7953_channel = ade7953_channel;
#endif
  mgos_set_timer(1000, MGOS_TIMER_REPEAT, shelly_sw_read_power, ctx);
#endif
  return svc;
}
