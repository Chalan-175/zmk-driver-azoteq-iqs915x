/* SPDX-License-Identifier: MIT */
#ifndef IQS915X_BASE_COUNT_H
#define IQS915X_BASE_COUNT_H

#include <iqs915x_channel_capture.h>
#include <zephyr/device.h>

struct iqs915x_base_count {
    int status, restore_status;
    uint32_t started_ms, ended_ms;
    uint16_t saved_target, measurement_flags, restore_flags;
    uint16_t values[IQS915X_CHANNEL_COUNT];
};

/* permitted() is checked throughout measurement. Restoration always runs. */
int iqs915x_base_count_start(const struct device *dev, bool (*permitted)(void));
int iqs915x_base_count_get(const struct device *dev, struct iqs915x_base_count *out);

#endif
