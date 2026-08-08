#pragma once
#include "usage_data.h"

// Background FreeRTOS task that polls the desk bridge over HTTP and keeps a
// mutex-guarded snapshot for the UI thread.
void usage_client_begin();

// Copies the latest snapshot. Never blocks for long.
void usage_client_get(UsageSnapshot &out);

// Wake the poll task immediately (button / pull-to-refresh).
void usage_client_refresh_now();
