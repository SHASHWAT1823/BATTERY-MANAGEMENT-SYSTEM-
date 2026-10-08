Automotive-Grade IoT Battery Management System (BMS)
### ESP32 Dual-Core | 16-Bit ADS1115 ADC | AIS-156 & ISO 26262 Compliance

Key Technical Highlights

* **Precision 16-Bit Analog Conversion:** Employs an external **ADS1115 ADC** (0.125 mV/LSB resolution) over standard 10-bit SAR ADCs to eliminate quantization noise and ensure accurate cell state tracking[cite: 1, 4].
* **AIS-156 Phase 2 Thermal Early Detection:** Implements early predictive thermal runaway detection via rolling differential rate-of-rise monitoring ($\Delta T / \Delta t \ge 0.40^\circ\text{C/s}$), triggering load cutoff well before reaching static limit thresholds[cite: 1, 4].
* **ISO 26262 ASIL-D Watchdog Supervisor:** FreeRTOS dual-task partitioning runs an independent supervisor task on **Core 0** that enforces hardware contactor lockout if the **Core 1** sensor acquisition loop experiences an I2C stall or latency $> 2.0\text{ s}$ (`DTC U0100`)[cite: 1, 4].
* **Dynamic State of Power (SOP / "Turtle Mode"):** Dynamically derates allowable drive power output ($0\text{--}100\%$) based on real-time cell thermal gradients and low state-of-charge conditions[cite: 1, 4].
* **Automotive Diagnostic Trouble Codes (DTCs):** Standard ISO 14229 / OBD-II fault code generation (`P0563`, `P0562`, `P0A93`, `P0A7F`, and `U0100`)[cite: 1, 4].
* **Synchronized Dual-Layer Visualization:**
  * **Local:** $16 \times 2$ I2C LCD character display for on-pack diagnostics[cite: 1, 4].
  * **Remote:** Embedded asynchronous Wi-Fi web server with interactive dynamic gauges and live Chart.js time-series streaming[cite: 1, 4].
