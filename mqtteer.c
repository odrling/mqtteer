
#include <cjson/cJSON.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc2/meminfo.h>
#include <libproc2/misc.h>
#include <locale.h>
#include <mosquitto.h>
#include <sensors/sensors.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOSQ_KEEPALIVE 90
#define DISCOVERY_TOPIC_PREFIX "homeassistant"
#define SENSORS_BUF_SIZE 200

#define RUNNING_ENTITY_NAME "running"

static char *mqtteer_device_name;
static struct mosquitto *mosq;
static int mqtteer_debug = 0;

#define NPSI_KINDS 3
static const char *PRESSURE_KINDS[NPSI_KINDS] = {"cpu", "memory", "io"};

void cleanup(void) {
  if (mosq != NULL)
    mosquitto_destroy(mosq);

  mosquitto_lib_cleanup();
}

void mqtteer_send(char *topic, char *payload) {
  int payload_len;
  int ret;

  payload_len = strlen(payload);
  ret = mosquitto_publish(mosq, NULL, topic, payload_len, payload, 0, false);
  if (ret != MOSQ_ERR_SUCCESS) {
    fprintf(stderr, "error %d", ret);
    exit(EXIT_FAILURE);
  }
}

int mqtteer_state_topic_len() {
  return strlen(mqtteer_device_name) +
         strlen(DISCOVERY_TOPIC_PREFIX "/sensor//state") + 1;
}

void mqtteer_get_state_topic_name(char *state_topic) {
  sprintf(state_topic, DISCOVERY_TOPIC_PREFIX "/sensor/%s/state",
          mqtteer_device_name);
}

int mqtteer_discovery_topic_len(char *name) {
  return strlen(name) + strlen(mqtteer_device_name) +
         strlen(DISCOVERY_TOPIC_PREFIX "/sensor///config") + 1;
}

void mqtteer_get_discovery_topic_name(char *state_topic, char *name) {
  sprintf(state_topic, DISCOVERY_TOPIC_PREFIX "/sensor/%s/%s/config",
          mqtteer_device_name, name);
}

int mqtteer_unique_id_len(char *name, char *device_name) {
  return strlen(name) + strlen(device_name) + 2;
}

void mqtteer_get_unique_id(char *unique_id, char *name, char *device_name) {
  sprintf(unique_id, "%s_%s", device_name, name);
}

void mqtteer_send_discovery(char *name, const char *device_class,
                            const char *unit_of_measurement) {
  char unique_id[mqtteer_unique_id_len(name, mqtteer_device_name)];
  mqtteer_get_unique_id(unique_id, name, mqtteer_device_name);
  char state_topic[mqtteer_state_topic_len()];
  mqtteer_get_state_topic_name(state_topic);

  char discovery_topic[mqtteer_discovery_topic_len(name)];
  mqtteer_get_discovery_topic_name(discovery_topic, name);

  cJSON *discovery_obj = cJSON_CreateObject();

  cJSON_AddStringToObject(discovery_obj, "name", name);
  cJSON_AddStringToObject(discovery_obj, "state_topic", state_topic);
  cJSON_AddStringToObject(discovery_obj, "unique_id", unique_id);

#define TEMPLATE_STR_FORMAT "{{ value_json['%s'] }}"
  char template_str[strlen(name) + strlen(TEMPLATE_STR_FORMAT) - 1];
  sprintf(template_str, TEMPLATE_STR_FORMAT, name);
  cJSON_AddStringToObject(discovery_obj, "value_template", template_str);

  if (device_class != NULL)
    cJSON_AddStringToObject(discovery_obj, "device_class", device_class);
  if (unit_of_measurement != NULL)
    cJSON_AddStringToObject(discovery_obj, "unit_of_measurement",
                            unit_of_measurement);

  cJSON *device_obj = cJSON_CreateObject();
  cJSON_AddStringToObject(device_obj, "name", mqtteer_device_name);

  cJSON *identifiers_arr = cJSON_CreateArray();
  cJSON_AddItemToArray(identifiers_arr,
                       cJSON_CreateString(mqtteer_device_name));

  cJSON_AddItemToObject(device_obj, "identifiers", identifiers_arr);
  cJSON_AddItemToObject(discovery_obj, "device", device_obj);

  char *discovery_payload = cJSON_Print(discovery_obj);
  if (mqtteer_debug)
    fprintf(stderr, "%s\n", discovery_payload);
  mqtteer_send(discovery_topic, discovery_payload);

  free(discovery_payload);
  cJSON_Delete(discovery_obj);
}

struct mqtteer_sensor {
  char *name;
  double value;
  const char *device_class;
  const char *unit;
};

// Device classes
static const char *TEMPERATURE = "temperature";

// UNITS
static const char *CELSIUS = "°C";

void mqtteer_sensor_free(struct mqtteer_sensor *sensor) {
  free(sensor->name);
  free(sensor);
}

void mqtteer_sensors_init() {
  int ret;

  ret = sensors_init(NULL);
  if (ret != 0) {
    perror("sensors_init failed");
    exit(EXIT_FAILURE);
  }
}

char *mqtteer_sensor_get_name(char *chip_name, char *label) {
  int len = strlen(chip_name) + strlen(label) + 2;
  char *name = malloc(len);

  snprintf(name, len, "%s_%s", chip_name, label);
  return name;
}

struct mqtteer_sensor *mqtteer_get_sensor(const struct sensors_chip_name *chip,
                                          int *nr_feat) {
  const sensors_feature *feature;
  const sensors_subfeature *sf;
  struct mqtteer_sensor *sensor;
  char name_buf[SENSORS_BUF_SIZE];
  char *label;
  char *sensor_name;

  sensors_snprintf_chip_name(name_buf, SENSORS_BUF_SIZE, chip);

  while ((feature = sensors_get_features(chip, nr_feat)) != NULL) {

    label = sensors_get_label(chip, feature);
    sensor_name = mqtteer_sensor_get_name(name_buf, label);
    free(label);

    switch (feature->type) {
    case SENSORS_FEATURE_TEMP:
      sf = sensors_get_subfeature(chip, feature, SENSORS_SUBFEATURE_TEMP_INPUT);
      if (sf) {
        sensor = malloc(sizeof(struct mqtteer_sensor));

        sensor->name = sensor_name;
        sensor->device_class = TEMPERATURE;
        sensor->unit = CELSIUS;
        sensors_get_value(chip, sf->number, &sensor->value);
        if (mqtteer_debug)
          fprintf(stderr, "found %s\n", sensor->name);

        return sensor;
      } else {
        if (mqtteer_debug)
          fprintf(stderr, "%s: could not get subfeature\n", sensor_name);
      }
      break;
    default:
      if (mqtteer_debug)
        fprintf(stderr, "%s: unsupported feature type: %d\n", sensor_name,
                feature->type);
    }

    free(sensor_name);
  }

  *nr_feat = 0;

  return NULL;
}

char *mqtteer_getenv(char *name) {
  char *value = getenv(name);
  if (value == NULL) {
    fprintf(stderr, "%s environment variable is not set", name);
    exit(EXIT_FAILURE);
  }
  return value;
}

struct mqtteer_battery {
  char *name;
  uint8_t capacity;
};

void mqtteer_free_battery(struct mqtteer_battery *battery) {
  free(battery->name);
  free(battery);
}

struct mqtteer_batteries {
  unsigned n;
  struct mqtteer_battery *batteries;
};

void mqtteer_free_batteries(struct mqtteer_batteries *batteries) {
  free(batteries->batteries);
  free(batteries);
}

#define POWER_SUPPLY_DIR "/sys/class/power_supply"
#define BATTERY_CAPACITY_NAME "capacity"
struct mqtteer_batteries *mqtteer_get_batteries() {
  struct dirent *power_supply;
  DIR *power_supplies_dir = opendir(POWER_SUPPLY_DIR);
  if (power_supplies_dir == NULL)
    perror("could not open " POWER_SUPPLY_DIR);

  struct mqtteer_batteries *batteries =
      malloc(sizeof(struct mqtteer_batteries));
  batteries->n = 0;
  batteries->batteries = NULL;
  int power_supplies_dir_fd = dirfd(power_supplies_dir);

  while ((power_supply = readdir(power_supplies_dir)) != NULL) {
    int power_supply_fd =
        openat(power_supplies_dir_fd, power_supply->d_name, O_RDONLY);

    if (power_supply_fd < 0) {
      fprintf(stderr, "could not open power supply subdirectory %s",
              power_supply->d_name);
      continue;
    }

    int power_supply_capacity_fd =
        openat(power_supply_fd, BATTERY_CAPACITY_NAME, O_RDONLY);

    if (power_supply_capacity_fd < 0) {
      if (errno == ENOENT) {
        if (mqtteer_debug)
          printf("skipping power supply %s: not a battery",
                 power_supply->d_name);

        goto ps_close;
      }
    }

    char buf[4];
    int readout = read(power_supply_capacity_fd, buf, 4);
    if (readout < 0) {
      perror("failed to read battery");
      goto ps_close_capacity;
    }

    char *endptr;
    uint8_t capacity = (uint8_t)strtol(buf, &endptr, 10);
    if (buf == endptr) {
      fprintf(stderr, "failed to parse battery capacity %s\n",
              power_supply->d_name);
      goto ps_close_capacity;
    }

    size_t new_size = (batteries->n + 1) * sizeof(struct mqtteer_battery);
    batteries->batteries = realloc(batteries->batteries, new_size);
    // probably should use model_name & serial_number
    batteries->batteries[batteries->n].name = strdup(power_supply->d_name);
    batteries->batteries[batteries->n].capacity = capacity;
    batteries->n++;

  ps_close_capacity:
    close(power_supply_capacity_fd);
  ps_close:
    close(power_supply_fd);
  }

  closedir(power_supplies_dir);
  return batteries;
}

struct mqtteer_psi_metrics {
  double avg10;
  double avg60;
  double avg300;
  long total;
};

struct mqtteer_psi {
  struct mqtteer_psi_metrics some;
  struct mqtteer_psi_metrics full;
};

int parsedblto(char **buf, double *metrics) {
  char *endptr = *buf;
  *metrics = strtod((*buf) + 2, &endptr);
  if (*buf == endptr) {
    fprintf(stderr, "failed to parse pressure %s\n", *buf);
    return -1;
  }

  *buf = endptr + 1;
  return 0;
}

int mqtteer_psi_set(char **buf, struct mqtteer_psi_metrics *psi_metrics) {
  while (**buf != '=')
    *buf += 1;
  *buf += 1;
  if (parsedblto(buf, &psi_metrics->avg10) < 0)
    goto failed;

  while (**buf != '=')
    *buf += 1;
  *buf += 1;
  if (parsedblto(buf, &psi_metrics->avg60))
    goto failed;

  while (**buf != '=')
    *buf += 1;
  *buf += 1;
  if (parsedblto(buf, &psi_metrics->avg300) < 0)
    goto failed;

  while (**buf != '=')
    *buf += 1;
  *buf += 1;
  char *endptr = *buf;
  psi_metrics->total = strtol(*buf, &endptr, 10);
  if (endptr == *buf)
    goto failed;

  *buf = endptr + 1;

  return 0;

failed:
  fprintf(stderr, "failed to parse pressure stall file %d %s\n", **buf, *buf);
  return -1;
}

#define PSI_DIR "/proc/pressure/"
#define PSI_BUF_SIZE 128
int mqtteer_psi_get(const char kind[], struct mqtteer_psi *psi) {
  char buf[PSI_BUF_SIZE];
  char psi_path[strlen(PSI_DIR) + strlen(kind) + 1];
  sprintf(psi_path, "%s%s", PSI_DIR, kind);

  int psi_fd = open(psi_path, O_RDONLY);
  if (psi_fd < 0) {
    perror("failed to open PSI");
    return psi_fd;
  }

  ssize_t count = read(psi_fd, buf, PSI_BUF_SIZE);
  if (count <= 0) {
    perror("failed to read PSI");
    return psi_fd;
  }

  char *bufpos = buf;

  while (bufpos[0] != '\0') {
    switch (bufpos[0]) {
    case 's':
      // assume some
      if (mqtteer_psi_set(&bufpos, &psi->some) < 0)
        return -1;
      break;
    case 'f':
      // assume full
      if (mqtteer_psi_set(&bufpos, &psi->full) < 0)
        return -1;
      break;
    default:
      fprintf(stderr, "unknown pressure type %s", bufpos);
    }
  }

  return 0;
}

void mqtteer_announce_topics() {
  int nr_chip = 0, nr_feat = 0;
  struct mqtteer_sensor *sensor;
  const struct sensors_chip_name *chip = NULL;
  if (mqtteer_debug)
    printf("announcing this device\n");

  mqtteer_send_discovery(RUNNING_ENTITY_NAME, NULL, NULL);
  mqtteer_send_discovery("uptime", "duration", "s");
  mqtteer_send_discovery("load1", "power_factor", NULL);
  mqtteer_send_discovery("load5", "power_factor", NULL);
  mqtteer_send_discovery("load15", "power_factor", NULL);
  mqtteer_send_discovery("used_memory", "data_size", "kB");
  mqtteer_send_discovery("total_memory", "data_size", "kB");

  // NOTE: PSI could be disabled on the system
  // in which case these entities would be marked "Unavailable"
  for (unsigned i = 0; i < NPSI_KINDS; i++) {
    char name[strlen("psi_memory_some_avg300") + 1]; // longest name

    sprintf(name, "psi_%s_some_avg10", PRESSURE_KINDS[i]);
    mqtteer_send_discovery(name, "power_factor", "%");
    sprintf(name, "psi_%s_some_avg60", PRESSURE_KINDS[i]);
    mqtteer_send_discovery(name, "power_factor", "%");
    sprintf(name, "psi_%s_some_avg300", PRESSURE_KINDS[i]);
    mqtteer_send_discovery(name, "power_factor", "%");
    sprintf(name, "psi_%s_some_total", PRESSURE_KINDS[i]);
    mqtteer_send_discovery(name, "power_factor", "ms");

    sprintf(name, "psi_%s_full_avg10", PRESSURE_KINDS[i]);
    mqtteer_send_discovery(name, "power_factor", "%");
    sprintf(name, "psi_%s_full_avg60", PRESSURE_KINDS[i]);
    mqtteer_send_discovery(name, "power_factor", "%");
    sprintf(name, "psi_%s_full_avg300", PRESSURE_KINDS[i]);
    mqtteer_send_discovery(name, "power_factor", "%");
    sprintf(name, "psi_%s_full_total", PRESSURE_KINDS[i]);
    mqtteer_send_discovery(name, "power_factor", "ms");
  }

  struct mqtteer_batteries *batteries = mqtteer_get_batteries();

  for (unsigned i = 0; i < batteries->n; i++) {
    mqtteer_send_discovery(batteries->batteries[i].name, "battery", "%");
  }

  mqtteer_sensors_init();
  while ((chip = sensors_get_detected_chips(NULL, &nr_chip)) != NULL) {
    while ((sensor = mqtteer_get_sensor(chip, &nr_feat)) != NULL) {
      mqtteer_send_discovery(sensor->name, sensor->device_class, sensor->unit);
      mqtteer_sensor_free(sensor);
    }
  }

  mqtteer_free_batteries(batteries);
  sensors_cleanup();
}

static void mqtteer_set_psi(const char *kind, cJSON *state_obj) {
  struct mqtteer_psi psi;
  int ret = mqtteer_psi_get(kind, &psi);
  if (ret < 0)
    return;

  char name[strlen("psi_memory_some_avg300") + 1]; // longest name

  sprintf(name, "psi_%s_some_avg10", kind);
  cJSON_AddNumberToObject(state_obj, name, psi.some.avg10);
  sprintf(name, "psi_%s_some_avg60", kind);
  cJSON_AddNumberToObject(state_obj, name, psi.some.avg60);
  sprintf(name, "psi_%s_some_avg300", kind);
  cJSON_AddNumberToObject(state_obj, name, psi.some.avg300);
  sprintf(name, "psi_%s_some_total", kind);
  cJSON_AddNumberToObject(state_obj, name, psi.some.total);

  sprintf(name, "psi_%s_full_avg10", kind);
  cJSON_AddNumberToObject(state_obj, name, psi.full.avg10);
  sprintf(name, "psi_%s_full_avg60", kind);
  cJSON_AddNumberToObject(state_obj, name, psi.full.avg60);
  sprintf(name, "psi_%s_full_avg300", kind);
  cJSON_AddNumberToObject(state_obj, name, psi.full.avg300);
  sprintf(name, "psi_%s_full_total", kind);
  cJSON_AddNumberToObject(state_obj, name, psi.full.total);
}

void mqtteer_report_metrics() {
  struct meminfo_info *meminfo = NULL;
  double uptime;
  double av1, av5, av15;
  unsigned long used, total;
  char state_topic[mqtteer_state_topic_len()];
  int nr_chip = 0, nr_feat = 0;
  const struct sensors_chip_name *chip = NULL;
  struct mqtteer_sensor *sensor;

  procps_loadavg(&av1, &av5, &av15);
  procps_uptime(&uptime, NULL);

  if (procps_meminfo_new(&meminfo) < 0) {
    fprintf(stderr, "failed to get memory info");
    exit(EXIT_FAILURE);
  }
  used = MEMINFO_GET(meminfo, MEMINFO_MEM_USED, ul_int);
  total = MEMINFO_GET(meminfo, MEMINFO_MEM_TOTAL, ul_int);
  procps_meminfo_unref(&meminfo);

  cJSON *state_obj = cJSON_CreateObject();
  cJSON_AddBoolToObject(state_obj, RUNNING_ENTITY_NAME, true);
  cJSON_AddNumberToObject(state_obj, "uptime", uptime);
  cJSON_AddNumberToObject(state_obj, "load1", av1);
  cJSON_AddNumberToObject(state_obj, "load5", av5);
  cJSON_AddNumberToObject(state_obj, "load15", av15);
  cJSON_AddNumberToObject(state_obj, "used_memory", used);
  cJSON_AddNumberToObject(state_obj, "total_memory", total);

  for (unsigned i = 0; i < NPSI_KINDS; i++)
    mqtteer_set_psi(PRESSURE_KINDS[i], state_obj);

  struct mqtteer_batteries *batteries = mqtteer_get_batteries();

  for (unsigned i = 0; i < batteries->n; i++) {
    cJSON_AddNumberToObject(state_obj, batteries->batteries[i].name,
                            batteries->batteries[i].capacity);
  }

  mqtteer_sensors_init();

  while ((chip = sensors_get_detected_chips(NULL, &nr_chip)) != NULL) {
    while ((sensor = mqtteer_get_sensor(chip, &nr_feat)) != NULL) {
      cJSON_AddNumberToObject(state_obj, sensor->name, sensor->value);
      mqtteer_sensor_free(sensor);
    }
  }
  sensors_cleanup();

  char *payload = cJSON_Print(state_obj);

  if (mqtteer_debug)
    printf("%s\n", payload);

  mqtteer_free_batteries(batteries);
  mqtteer_get_state_topic_name(state_topic);
  mqtteer_send(state_topic, payload);
  free(payload);
  cJSON_Delete(state_obj);
}

void mqtteer_set_will() {
  char payload[] = "{\"" RUNNING_ENTITY_NAME "\":false}";
  int payload_len = strlen(payload);

  char state_topic[mqtteer_state_topic_len()];
  mqtteer_get_state_topic_name(state_topic);

  mosquitto_will_set(mosq, state_topic, payload_len, payload, 0, false);
}

int main(void) {
  int mosq_port;

  char *mosq_username = mqtteer_getenv("MQTTEER_USERNAME");
  char *mosq_password = mqtteer_getenv("MQTTEER_PASSWORD");
  char *mosq_host = mqtteer_getenv("MQTTEER_HOST");
  char *mosq_port_str = getenv("MQTTEER_PORT");
  mqtteer_debug = getenv("MQTTEER_DEBUG") != NULL;

  char *port_endptr;

  if (mosq_port_str == NULL)
    mosq_port = 1883;
  else {
    mosq_port = strtol(mosq_port_str, &port_endptr, 10);
    if (mosq_port_str == port_endptr || mosq_port == 0) {
      fprintf(stderr, "MQTTEER_PORT is invalid: %s", mosq_port_str);
      exit(EXIT_FAILURE);
    }
  }

  mqtteer_device_name = mqtteer_getenv("MQTTEER_DEVICE_NAME");

  mosquitto_lib_init();
  atexit(cleanup);
  mosq = mosquitto_new(mqtteer_device_name, true, NULL);
  mosquitto_username_pw_set(mosq, mosq_username, mosq_password);
  mqtteer_set_will();
  mosquitto_connect(mosq, mosq_host, mosq_port, MOSQ_KEEPALIVE);

  while (true) {
    mqtteer_announce_topics();
    mqtteer_report_metrics();
    sleep(60);
  }

  exit(EXIT_SUCCESS);
}
