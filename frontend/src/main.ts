interface TelemetryData {
  rpm: number;
  speed_kmh: number;
  gear: number;
  oil_temp_c: number;
  coolant_temp_c: number;
  battery_voltage: number;
  odometer_km: number;
  fuel_range_km: number;
  fuel_level_pct: number;
  mgct_level: number;
  abs_mode: number;
  riding_mode: number;
  front_brake_bar: number;
  accel_g: number;
  mil_active: boolean;
  warning_active: boolean;
  oil_alarm_active: boolean;
  service_wrench_active: boolean;
  timestamp: number;
}

let ws: WebSocket | null = null;
let reconnectTimer: number | null = null;
const logHistory: string[] = [];
const MAX_LOG_LINES = 25;

function initApp() {
  const statusBadge = document.getElementById('conn-status')!;
  const valSpeed = document.getElementById('val-speed')!;
  const valGear = document.getElementById('val-gear')!;
  const valRpm = document.getElementById('val-rpm')!;
  const tachoFill = document.getElementById('tacho-fill')!;
  const valOil = document.getElementById('val-oil')!;
  const valCoolant = document.getElementById('val-coolant')!;
  const valBattery = document.getElementById('val-battery')!;
  const valOdo = document.getElementById('val-odo')!;
  const valMgct = document.getElementById('val-mgct')!;
  const valAbs = document.getElementById('val-abs')!;
  const logConsole = document.getElementById('log-console')!;
  const actionFeedback = document.getElementById('action-feedback')!;

  const lampMil = document.getElementById('lamp-mil')!;
  const lampOil = document.getElementById('lamp-oil')!;
  const lampWrench = document.getElementById('lamp-wrench')!;
  const lampWarning = document.getElementById('lamp-warning')!;

  const btnServiceReset = document.getElementById('btn-service-reset') as HTMLButtonElement;
  const btnClearDtc = document.getElementById('btn-clear-dtc') as HTMLButtonElement;

  function appendLog(line: string) {
    logHistory.unshift(line);
    if (logHistory.length > MAX_LOG_LINES) {
      logHistory.pop();
    }
    logConsole.textContent = logHistory.join('\n');
  }

  function connectWebSocket() {
    if (reconnectTimer !== null) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }

    const wsProtocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${wsProtocol}//${window.location.host}/ws`;
    ws = new WebSocket(wsUrl);

    ws.onopen = () => {
      statusBadge.textContent = 'CONNECTED';
      statusBadge.classList.add('connected');
      appendLog('[WS] Connected to ESP32 CAN Telemetry Stream.');
    };

    ws.onmessage = (event) => {
      try {
        const data: TelemetryData = JSON.parse(event.data);
        
        // Update Gauges
        valSpeed.textContent = Math.round(data.speed_kmh).toString();
        valGear.textContent = data.gear === 0 ? 'N' : data.gear.toString();
        valRpm.textContent = `${data.rpm} RPM`;

        // Tachometer Bar Fill (max 8000 RPM)
        const pct = Math.min(100, Math.max(0, (data.rpm / 8000) * 100));
        tachoFill.style.width = `${pct}%`;
        if (data.rpm > 6500) {
          tachoFill.style.background = '#ef4444';
        } else if (data.rpm > 4500) {
          tachoFill.style.background = '#f59e0b';
        } else {
          tachoFill.style.background = 'linear-gradient(to right, #22c55e, #f59e0b)';
        }

        // Warning Lamps
        lampMil.classList.toggle('active-mil', Boolean(data.mil_active));
        lampOil.classList.toggle('active-oil', Boolean(data.oil_alarm_active));
        lampWrench.classList.toggle('active-wrench', Boolean(data.service_wrench_active));
        lampWarning.classList.toggle('active-warning', Boolean(data.warning_active));

        // Metrics
        valOil.textContent = `${data.oil_temp_c} °C`;
        valCoolant.textContent = `${data.coolant_temp_c} °C`;
        valBattery.textContent = `${data.battery_voltage.toFixed(1)} V`;
        valOdo.textContent = `${data.odometer_km} km`;
        valMgct.textContent = `Level ${data.mgct_level}`;
        valAbs.textContent = `Map ${data.abs_mode}`;

        // Stream Log (Capped Buffer)
        appendLog(`[${new Date().toLocaleTimeString()}] RPM: ${data.rpm} | Speed: ${data.speed_kmh.toFixed(1)} km/h | Gear: ${data.gear} | Coolant: ${data.coolant_temp_c}°C | Battery: ${data.battery_voltage.toFixed(1)}V`);
      } catch (err) {
        console.error('Failed to parse WebSocket JSON payload', err);
      }
    };

    ws.onclose = () => {
      statusBadge.textContent = 'DISCONNECTED';
      statusBadge.classList.remove('connected');
      if (reconnectTimer === null) {
        reconnectTimer = window.setTimeout(connectWebSocket, 2000);
      }
    };
  }

  // Diagnostic API Triggers
  btnServiceReset.addEventListener('click', async () => {
    actionFeedback.textContent = 'Sending UDS Service Reset command to 0x720...';
    try {
      const res = await fetch('/api/service/reset', { method: 'POST' });
      const json = await res.json();
      actionFeedback.textContent = `✓ ${json.message}`;
    } catch (e) {
      actionFeedback.textContent = '❌ Failed to send Service Reset request.';
    }
  });

  btnClearDtc.addEventListener('click', async () => {
    actionFeedback.textContent = 'Sending UDS Clear DTC command to 0x7E0...';
    try {
      const res = await fetch('/api/ecu/clear-dtc', { method: 'POST' });
      const json = await res.json();
      actionFeedback.textContent = `✓ ${json.message}`;
    } catch (e) {
      actionFeedback.textContent = '❌ Failed to send Clear DTC request.';
    }
  });

  connectWebSocket();
}

document.addEventListener('DOMContentLoaded', initApp);
