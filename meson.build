project('mqtteer', 'c', version: '2.0.1')

mosquitto = dependency('libmosquitto')
proc2 = dependency('libproc2')
cjson = dependency('libcjson')

cc = meson.get_compiler('c')
sensors = cc.find_library('sensors', required: true)

mqtteer_exe = executable(
    'mqtteer',
    'mqtteer.c',
    dependencies: [cjson, proc2, mosquitto, sensors],
    install: true,
)
