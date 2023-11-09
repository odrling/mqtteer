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
uptime of the computer it is running on.

It uses the following environment variables to configure itself:

* MOSQ_HOST: MQTT host to connect to
* MOSQ_PORT: port to connect to on the host (defaults to 1883)
* MOSQ_USERNAME: MQTT username of the client
* MOSQ_PASSWORD: MQTT password of the client
* MQTTEER_DEVICE_NAME: name that this device will have in Home Assistant

Currently, the values sent on the first run of mqtteer might not get registered
in Home Assistant.
