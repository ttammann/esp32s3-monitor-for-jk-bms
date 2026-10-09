// The 55AA listener: owns UART1, decodes the BMS's push stream and publishes
// it. Never returns; never transmits.
#pragma once

void jk_listen_run(void);
