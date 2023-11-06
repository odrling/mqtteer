project('mqtteer', 'c')

mosquitto = dependency('libmosquitto')
proc2 = dependency('libproc2')
cjson = dependency('libcjson')

mqtteer_exe = executable(
    'mqtteer',
    'mqtteer.c',
    dependencies: [cjson, proc2, mosquitto],
    install: true,
)
