#include "esp_ota_ops.h"
static const esp_partition_t s_part = { .label = "ota_0" };
const esp_partition_t *esp_ota_get_running_partition(void) { return &s_part; }
