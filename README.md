# mqtteer

An MQTT client to send device metrics to Home Assistant

## Compiling

Setup meson to build mqtteer in a `build` subdirectory:
```
$ meson setup build
$ meson compile -C build
```

## Features

When launched, mqtteer reports the current load average, memory usage and
uptime and temperature sensors (as found by lm-sensors) of the computer it is
running on.

It uses the following environment variables to configure itself:

* MQTTEER_HOST: MQTT host to connect to
* MQTTEER_PORT: port to connect to on the host (defaults to 1883)
* MQTTEER_USERNAME: MQTT username of the client
* MQTTEER_PASSWORD: MQTT password of the client
* MQTTEER_DEVICE_NAME: name that this device will have in Home Assistant
* MQTTEER_DEBUG: print a lot of debugging information when defined
