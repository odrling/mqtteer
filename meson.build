project(
    'mqtteer',
    'c',
    version: '2.2.10',
    license: 'MIT',
    default_options: ['warning_level=2', 'werror=true', 'c_std=gnu17'],
)

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
