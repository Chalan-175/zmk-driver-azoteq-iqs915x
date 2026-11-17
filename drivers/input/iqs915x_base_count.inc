/* SPDX-License-Identifier: MIT */

int iqs915x_base_count_start(const struct device *dev, bool (*permitted)(void))
{
  if (!dev || !permitted || !device_is_ready(dev)) return -EINVAL;
  if (k_is_in_isr()) return -EWOULDBLOCK;
  struct iqs915x_data *d = dev->data;
  if (atomic_get(&d->lifecycle) != IQS915X_READY) return -ENODEV;
  if (!permitted() || atomic_get(&d->pm_suspended)) return -EACCES;
  if (k_mutex_lock(&d->channel_lock, K_NO_WAIT)) return -EBUSY;
  int rc = 0;
  if (atomic_get(&d->diagnostic_job) || d->channel_snapshot.status == -EINPROGRESS) {
    rc = -EBUSY;
  } else {
    d->base_count = (struct iqs915x_base_count){.status = -EINPROGRESS,
                                              .restore_status = -ENODATA};
    d->base_permitted = permitted;
    d->channel_requested_generation = iqs915x_request_generation(d);
    atomic_set(&d->diagnostic_job, IQS915X_DIAG_BASE_COUNT);
    atomic_set(&d->channel_pending, 1);
    k_sem_give(&d->rdy_sem);
  }
  k_mutex_unlock(&d->channel_lock);
  return rc;
}

int iqs915x_base_count_get(const struct device *dev, struct iqs915x_base_count *out)
{
  if (!dev || !out || !device_is_ready(dev)) return -EINVAL;
  struct iqs915x_data *d = dev->data;
  k_mutex_lock(&d->channel_lock, K_FOREVER);
  if (atomic_get(&d->diagnostic_job) == IQS915X_DIAG_BASE_COUNT) {
    k_mutex_unlock(&d->channel_lock);
    return -EINPROGRESS;
  }
  *out = d->base_count;
  k_mutex_unlock(&d->channel_lock);
  return 0;
}

static bool base_count_permitted(struct iqs915x_data *d)
{
  return d->base_permitted() && !atomic_get(&d->pm_suspended) &&
         atomic_get(&d->lifecycle) == IQS915X_READY &&
         d->channel_requested_generation == iqs915x_request_generation(d);
}

/* Give STOP time to close the preceding communication window. */
static void base_count_pace(struct iqs915x_data *d)
{
  if (k_uptime_get() <= d->comm_completed_ms) k_sleep(K_MSEC(1));
}

static int base_count_read16(struct iqs915x_data *d, uint16_t reg, uint16_t *value)
{
  base_count_pace(d);
  return iqs915x_read_reg16(d->dev, reg, value);
}

static int base_count_set(struct iqs915x_data *d, uint16_t reg, uint16_t value)
{
  base_count_pace(d);
  int rc = iqs915x_write_reg16(d->dev, reg, value);
  if (rc) return rc;
  uint16_t observed;
  rc = base_count_read16(d, reg, &observed);
  return rc ? rc : (observed == value ? 0 : -EIO);
}

static int base_count_ati(struct iqs915x_data *d, uint16_t *flags, bool measuring)
{
  const struct iqs915x_config *cfg = d->dev->config;
  uint16_t stale;
  int rc = base_count_read16(d, IQS915X_INFO_FLAGS, &stale);
  if (rc) return rc;
  if (stale == 0xEEEE || (stale & IQS915X_SHOW_RESET)) return -EIO;
  if (measuring && !base_count_permitted(d)) return -ECANCELED;
  base_count_pace(d);
  rc = iqs915x_write_reg16(d->dev, IQS915X_SYSTEM_CONTROL, IQS915X_MODE_ACTIVE | IQS915X_REATI_TP);
  if (rc) return rc;

  int64_t deadline = k_uptime_get() + 5000;
  while (k_uptime_get() < deadline) {
    if (measuring && !base_count_permitted(d)) return -ECANCELED;
    int rdy = gpio_pin_get_dt(&cfg->rdy_gpio);
    if (rdy < 0) return rdy;
    if (rdy > 0) {
      uint16_t info;
      rc = base_count_read16(d, IQS915X_INFO_FLAGS, &info);
      if (rc) return rc;
      if (info != 0xEEEE) {
        *flags = info;
        if (info & IQS915X_SHOW_RESET) return -EIO;
        if (info & IQS915X_REATI_OCCURRED) return 0;
      }
    }
    k_sleep(K_MSEC(5));
  }
  return -ETIMEDOUT;
}

static int base_count_measure(struct iqs915x_data *d, struct iqs915x_base_count *result)
{
  int rc = base_count_ati(d, &result->measurement_flags, true);
  int64_t deadline = k_uptime_get() + 2000;
  uint8_t bytes[72];
  for (unsigned offset = 0; !rc && offset < IQS915X_CHANNEL_COUNT; offset += sizeof(bytes) / 2) {
    if (!base_count_permitted(d)) return -ECANCELED;
    if (k_uptime_get() >= deadline) return -ETIMEDOUT;
    unsigned count = MIN(sizeof(bytes) / 2, IQS915X_CHANNEL_COUNT - offset);
    base_count_pace(d);
    rc = iqs915x_read_block(d->dev, 0xA000 + 2 * offset, bytes, 2 * count);
    if (!rc) {
      for (unsigned i = 0; i < count; i++) {
        uint16_t value = iqs_channel_u16(bytes + 2 * i);
        if (value == 0xEEEE) return -EIO;
        result->values[offset + i] = value;
      }
    }
  }
  if (rc) return rc;
  uint16_t info;
  rc = base_count_read16(d, IQS915X_INFO_FLAGS, &info);
  if (rc) return rc;
  if (info == 0xEEEE || (info & (IQS915X_SHOW_RESET | IQS915X_REATI_OCCURRED))) return -EIO;
  return base_count_permitted(d) ? 0 : -ECANCELED;
}

static void iqs915x_base_count_execute(struct iqs915x_data *d)
{
  struct iqs915x_base_count *result = &d->base_count;
  result->started_ms = k_uptime_get_32();
  bool mode_attempted = base_count_permitted(d);
  bool config_attempted = false, target_attempted = false;
  uint16_t config = 0;
  int rc = -ECANCELED, restore = 0;

  if (mode_attempted) {
    atomic_set(&d->channel_running, 1);
    atomic_clear(&d->output_enabled);
    d->enabled = false;
    iqs915x_reset_input_session(d);
    iqs915x_begin_mode(d, IQS915X_MODE_ACTIVE, false);
    rc = diagnostic_mode_finish(d, k_uptime_get() + 3000);
    if (!rc) d->ic_suspended = false;
  }
  if (!rc && !base_count_permitted(d)) rc = -ECANCELED;
  if (!rc) rc = base_count_read16(d, IQS915X_CONFIG_SETTINGS, &config);
  if (!rc && config == 0xEEEE) rc = -EIO;
  if (!rc) rc = base_count_read16(d, IQS915X_TP_ATI_TARGET, &result->saved_target);
  if (!rc && (!result->saved_target || result->saved_target == 0xEEEE)) rc = -EIO;
  uint16_t geometry;
  if (!rc) rc = base_count_read16(d, IQS915X_TRACKPAD_SETTINGS + 1, &geometry);
  if (!rc && geometry != (IQS915X_CHANNEL_ROWS << 8 | IQS915X_CHANNEL_COLUMNS)) rc = -ENOTSUP;
  if (!rc && !base_count_permitted(d)) rc = -ECANCELED;
  if (!rc) {
    config_attempted = true; /* Even an errored write may be accepted by the IC. */
    rc = base_count_set(d, IQS915X_CONFIG_SETTINGS,
                        (config | IQS915X_MANUAL_CONTROL) &
                        ~(IQS915X_EVENT_MODE | IQS915X_TP_REATI_ENABLE | IQS915X_ALP_REATI_ENABLE));
  }
  if (!rc && !base_count_permitted(d)) rc = -ECANCELED;
  if (!rc) {
    target_attempted = true;
    rc = base_count_set(d, IQS915X_TP_ATI_TARGET, 0);
  }
  if (!rc) rc = base_count_measure(d, result);

  if (target_attempted) {
    restore = base_count_set(d, IQS915X_TP_ATI_TARGET, result->saved_target);
    if (!restore) restore = base_count_ati(d, &result->restore_flags, false);
    if (!restore && (result->restore_flags & IQS915X_ATI_ERROR)) restore = -EIO;
  }
  if (config_attempted) {
    int config_rc = base_count_set(d, IQS915X_CONFIG_SETTINGS, config);
    if (!restore) restore = config_rc;
  }
  if (mode_attempted && !restore && atomic_get(&d->lifecycle) == IQS915X_READY) {
    if (target_attempted) iqs915x_runtime_info(d, result->restore_flags);
    d->applied_generation = iqs915x_request_generation(d) - 1;
    iqs915x_apply_pending_power_request(d);
    restore = diagnostic_mode_finish(d, k_uptime_get() + 3000);
  } else if (mode_attempted && !restore) {
    restore = -EIO;
  }

  if (restore) {
    /* Do not retain an ambiguous Target/configuration via the warm-start path. */
    iqs915x_runtime_reset(d, "base-count-restore");
    d->init_step = INIT_SOFTWARE_RESET;
  }
  k_mutex_lock(&d->channel_lock, K_FOREVER);
  result->status = rc;
  result->restore_status = restore;
  result->ended_ms = k_uptime_get_32();
  atomic_clear(&d->diagnostic_job);
  atomic_clear(&d->channel_running);
  k_mutex_unlock(&d->channel_lock);
}
