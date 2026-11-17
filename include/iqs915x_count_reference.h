/* SPDX-License-Identifier: MIT */
#ifndef IQS915X_COUNT_REFERENCE_H
#define IQS915X_COUNT_REFERENCE_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

#define IQS915X_COUNT_BLOCK_MAX 36

struct iqs915x_count_request {
    uint32_t capture_id;
    uint16_t offset;
    uint8_t count;
    bool reference;
};

/* Optional application hooks. Only called on the normal Active sensor thread.
 * A finite deadline can wake Event Mode for a diagnostic read, one STOP per
 * read. The application must advance the deadline after every result. */
int64_t iqs915x_count_plan(const struct device *dev, uint32_t generation,
                          uint32_t ati_sequence, struct iqs915x_count_request *request);
void iqs915x_count_result(const struct device *dev, const struct iqs915x_count_request *request,
                          uint32_t started_ms, uint32_t ended_ms, int error,
                          const uint8_t *little_endian);

#endif
