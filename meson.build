project('mqtteer', 'c')

mosquitto = dependency('libmosquitto')
proc2 = dependency('libproc2')
cjson = dependency('libcjson')

example = executable(
    'mqtteer',
    'mqtteer.c',
    dependencies: [cjson, proc2, mosquitto],
)
