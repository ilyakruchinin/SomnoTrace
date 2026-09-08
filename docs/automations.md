# Smart Home & Home Assistant Automations

> Transform your ResMed AirSense 11 into a first-class smart home device! With SomnoTrace, you can trigger bedroom bedtime routines, track insurance compliance, and receive sleep alerts directly in Home Assistant, Apple Home, or Node-RED.

---

## What You Can Automate

By integrating SomnoTrace into your smart home system, you can build automations like:

- 🌙 **"Goodnight" Bedtime Scene:** Automatically turn off bedroom lights, close motorized blinds, set the thermostat to a cool sleep temperature, and silence phone notifications the instant you put on your mask and start therapy.
- 🚨 **Compliance & Early Wakeup Guardian:** If therapy unexpectedly stops before reaching 4 hours of sleep (e.g. your mask slips off during deep sleep), receive an immediate notification, vibrate a smartwatch, or trigger a smart bed shaker before you lose insurance compliance.
- 🌅 **"Good Morning" Wakeup Scene:** When you take off your mask and stop therapy in the morning (after 6:00 AM), turn on warm ambient lights, open blinds, start your coffee maker, and read out the morning weather.
- 🔌 **Smart Plug & Humidifier Management:** Automatically turn on a bedside humidifier or cooling fan when CPAP therapy begins, and power it off when you wake up.

---

## Integration Methods

### Method 1: Instant Push Triggers via `ntfy` / Webhooks (Recommended)

SomnoTrace has built-in support for **[ntfy](https://ntfy.sh)** push notifications. You can point SomnoTrace to any public or self-hosted ntfy instance:

1. In the SomnoTrace web portal (`http://somnotrace.local`), go to **Therapy Alert**.
2. Enable **Push Notification** and set your server (e.g., `https://ntfy.sh`) and a unique topic (e.g., `my-cpap-alert-98234`).
3. In **Home Assistant**, subscribe to the ntfy topic using the [ntfy integration](https://www.home-assistant.io/integrations/notify/) or a Webhook automation.

Whenever therapy stops unexpectedly, Home Assistant receives an instant webhook event.

---

### Method 2: Home Assistant REST Sensor (Live Status Monitoring)

You can poll SomnoTrace's `/api/status` endpoint to create live sensors in Home Assistant.

Add the following to your Home Assistant `configuration.yaml`:

```yaml
sensor:
  - platform: rest
    name: "SomnoTrace Status"
    resource: "http://somnotrace.local/api/status"
    scan_interval: 10
    json_attributes:
      - ble
      - wifi
      - battery
      - alert
      - uptime
    value_template: "{{ value_json.ble.state }}"

  - platform: template
    sensors:
      cpap_connected:
        friendly_name: "CPAP Connected"
        value_template: "{{ states('sensor.somnotrace_status') == 'paired' }}"
        icon_template: "mdi:bluetooth"

      cpap_alert_state:
        friendly_name: "CPAP Alert State"
        value_template: "{{ state_attr('sensor.somnotrace_status', 'alert').state }}"
        icon_template: "mdi:alarm-light"

      somnotrace_battery:
        friendly_name: "SomnoTrace Battery"
        unit_of_measurement: "%"
        device_class: battery
        value_template: "{{ state_attr('sensor.somnotrace_status', 'battery').percent }}"
```

---

### Method 3: Remote Control Switch (Start / Stop Therapy)

You can create a switch in Home Assistant to start or stop therapy on the AirSense 11 using SomnoTrace's [RPC Bridge](rpc-bridge.md).

Add the following to your Home Assistant `configuration.yaml`:

```yaml
rest_command:
  cpap_start_therapy:
    url: "http://somnotrace.local/api/ble/passthrough"
    method: "POST"
    headers:
      Content-Type: "application/json"
    payload: '{"id":10,"jsonrpc":"1.0","method":"EnterTherapy"}'

  cpap_stop_therapy:
    url: "http://somnotrace.local/api/ble/passthrough"
    method: "POST"
    headers:
      Content-Type: "application/json"
    payload: '{"id":11,"jsonrpc":"1.0","method":"EnterStandby"}'
```

---

## Example Automations (Home Assistant YAML)

### 1. Bedtime Scene Trigger (Lights Off when CPAP Starts)

```yaml
alias: "Bedtime: Turn off bedroom lights when CPAP starts"
description: "Triggers when CPAP alert enters armed state"
trigger:
  - platform: state
    entity_id: sensor.cpap_alert_state
    to: "armed"
condition:
  - condition: time
    after: "21:00:00"
    before: "05:00:00"
action:
  - service: light.turn_off
    target:
      area_id: bedroom
  - service: notify.notify
    data:
      message: "Sleep well! CPAP therapy has started."
```

---

### 2. Emergency Mask-Off / Interruption Alert (Vibrate Phone & Smartwatch)

```yaml
alias: "Alert: CPAP therapy unexpectedly stopped"
description: "Triggers when therapy stops during the night"
trigger:
  - platform: state
    entity_id: sensor.cpap_alert_state
    to: "pending"
  - platform: state
    entity_id: sensor.cpap_alert_state
    to: "push_sent"
condition:
  - condition: time
    after: "22:00:00"
    before: "06:00:00"
action:
  - service: notify.mobile_app_your_phone
    data:
      title: "⚠️ CPAP Alert: Therapy Stopped"
      message: "Your CPAP mask came off or therapy stopped. Put your mask back on to maintain compliance!"
      data:
        push:
          sound: "critical"
        actions:
          - action: "SILENCE_ALERT"
            title: "I am Awake"
```

---

### 3. Morning Wakeup Routine

```yaml
alias: "Morning: CPAP stopped after 6 AM"
description: "Gentle wakeup when taking off mask in the morning"
trigger:
  - platform: state
    entity_id: sensor.cpap_alert_state
    from: "armed"
    to: "disarmed"
condition:
  - condition: time
    after: "06:00:00"
    before: "11:00:00"
action:
  - service: light.turn_on
    target:
      entity_id: light.bedroom_lamp
    data:
      brightness_pct: 40
      transition: 5
```
