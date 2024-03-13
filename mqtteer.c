
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

// mqtteer should fail fast

static inline void cclose(int fd) {
  if (close(fd) != 0) {
    perror("close failed");
    exit(-1);
  }
}

static inline void cclosedir(DIR *dir) {
  if (closedir(dir) != 0) {
    perror("closedir failed");
    exit(-1);
  }
}

static inline void *mmalloc(int len) {
  void *ptr = malloc(len);
  if (ptr == NULL) {
    perror("malloc failed");
    exit(-1);
  }
  return ptr;
}

static inline void *rrealloc(void *ptr, int len) {
  void *out_ptr = realloc(ptr, len);
  if (out_ptr == NULL) {
    perror("realloc failed");
    exit(-1);
  }
  return out_ptr;
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

union mqtteer_value {
  double dblval;
  long lval;
  unsigned ulval;
  int ival;
  char *strval;
};

enum mqtteer_valtype {
  MQTTEER_TYPE_DOUBLE = 1,
  MQTTEER_TYPE_LONG = 2,
  MQTTEER_TYPE_UNSIGNED_LONG = 3,
  MQTTEER_TYPE_INT = 4,
  MQTTEER_TYPE_STR = 5,
};

typedef struct {
  char *name;
  union mqtteer_value *value;
  enum mqtteer_valtype value_type;
  const char *device_class;
  const char *unit_of_measurement;
} mqtteer_report;

typedef struct {
  int nb;
  mqtteer_report *reports;
} mqtteer_reports;

void mqtteer_free_report(mqtteer_report report) {
  free(report.name);
  if (report.value_type == MQTTEER_TYPE_STR) {
    free(report.value->strval);
  }
  free(report.value);
}

void mqtteer_free_reports(mqtteer_reports *reports) {
  for (int i = 0; i < reports->nb; i++)
    mqtteer_free_report(reports->reports[i]);

  free(reports->reports);
  free(reports);
}

void mqtteer_new_report(mqtteer_reports *reports, char *name,
                        union mqtteer_value *value,
                        enum mqtteer_valtype value_type, const char *ha_kind,
                        const char *unit_of_measurement) {
  reports->nb++;
  int new_size = sizeof(mqtteer_report) * reports->nb;
  reports->reports = rrealloc(reports->reports, new_size);
  mqtteer_report *report = &reports->reports[reports->nb - 1];

  report->name = strdup(name);
  report->value = value;
  report->value_type = value_type;
  report->device_class = ha_kind;
  report->unit_of_measurement = unit_of_measurement;
}

void mqtteer_new_report_dbl(mqtteer_reports *reports, char *name, double value,
                            const char *ha_kind,
                            const char *unit_of_measurement) {
  union mqtteer_value *val = malloc(sizeof(union mqtteer_value));
  val->dblval = value;
  mqtteer_new_report(reports, name, val, MQTTEER_TYPE_DOUBLE, ha_kind,
                     unit_of_measurement);
}

void mqtteer_new_report_int(mqtteer_reports *reports, char *name, int value,
                            const char *ha_kind,
                            const char *unit_of_measurement) {
  union mqtteer_value *val = malloc(sizeof(union mqtteer_value));
  val->ival = value;
  mqtteer_new_report(reports, name, val, MQTTEER_TYPE_INT, ha_kind,
                     unit_of_measurement);
}

void mqtteer_new_report_long(mqtteer_reports *reports, char *name, long value,
                             const char *ha_kind,
                             const char *unit_of_measurement) {
  union mqtteer_value *val = malloc(sizeof(union mqtteer_value));
  val->lval = value;
  mqtteer_new_report(reports, name, val, MQTTEER_TYPE_LONG, ha_kind,
                     unit_of_measurement);
}

void mqtteer_new_report_ulong(mqtteer_reports *reports, char *name,
                              unsigned long value, const char *ha_kind,
                              const char *unit_of_measurement) {
  union mqtteer_value *val = malloc(sizeof(union mqtteer_value));
  val->ulval = value;
  mqtteer_new_report(reports, name, val, MQTTEER_TYPE_UNSIGNED_LONG, ha_kind,
                     unit_of_measurement);
}

void mqtteer_new_report_str(mqtteer_reports *reports, char *name, char *value,
                            const char *ha_kind,
                            const char *unit_of_measurement) {
  union mqtteer_value *val = malloc(sizeof(union mqtteer_value));
  val->strval = value;
  mqtteer_new_report(reports, name, val, MQTTEER_TYPE_STR, ha_kind,
                     unit_of_measurement);
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
  char *name = mmalloc(len);

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
        sensor = mmalloc(sizeof(struct mqtteer_sensor));

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
  int capacity;
};

void mqtteer_free_battery(struct mqtteer_battery *battery) {
  free(battery->name);
  free(battery);
}

typedef struct {
  unsigned n;
  struct mqtteer_battery *batteries;
} mqtteer_batteries;

void mqtteer_free_batteries(mqtteer_batteries *batteries) {
  if (batteries->batteries != NULL)
    free(batteries->batteries);
  free(batteries);
}

#define POWER_SUPPLY_DIR "/sys/class/power_supply"
#define BATTERY_CAPACITY_NAME "capacity"
mqtteer_batteries *mqtteer_get_batteries() {
  struct dirent *power_supply;
  DIR *power_supplies_dir = opendir(POWER_SUPPLY_DIR);
  if (power_supplies_dir == NULL)
    perror("could not open " POWER_SUPPLY_DIR);

  mqtteer_batteries *batteries = mmalloc(sizeof(mqtteer_batteries));
  batteries->n = 0;
  batteries->batteries = NULL;

  if (power_supplies_dir == NULL)
    return batteries;

  int power_supplies_dir_fd = dirfd(power_supplies_dir);

  while ((power_supply = readdir(power_supplies_dir)) != NULL) {
    if (power_supply->d_name[0] == '.')
      continue;

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
          printf("skipping power supply %s: not a battery\n",
                 power_supply->d_name);
      } else {
        perror("could not read battery capacity");
      }

      goto ps_close;
    }

    char buf[4];
    ssize_t readout = read(power_supply_capacity_fd, buf, 4);
    buf[readout] = '\0';
    if (readout < 0) {
      perror("failed to read battery");
      goto ps_close_capacity;
    }

    char *endptr;
    int capacity = (int)strtol(buf, &endptr, 10);
    if (buf == endptr) {
      fprintf(stderr, "failed to parse battery capacity %s\n",
              power_supply->d_name);
      goto ps_close_capacity;
    }

    size_t new_size = (batteries->n + 1) * sizeof(struct mqtteer_battery);
    batteries->batteries = rrealloc(batteries->batteries, new_size);
    // probably should use model_name & serial_number
    batteries->batteries[batteries->n].name = strdup(power_supply->d_name);
    batteries->batteries[batteries->n].capacity = capacity;
    batteries->n++;

  ps_close_capacity:
    cclose(power_supply_capacity_fd);
  ps_close:
    cclose(power_supply_fd);
  }

  cclosedir(power_supplies_dir);
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
  *metrics = strtod((*buf), &endptr);
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

  ssize_t count = read(psi_fd, buf, PSI_BUF_SIZE - 1);
  buf[count] = '\0';
  if (count <= 0) {
    perror("failed to read PSI");
    goto err_return;
  }

  char *bufpos = buf;

  while (bufpos[0] != '\0') {
    switch (bufpos[0]) {
    case 's':
      // assume some
      if (mqtteer_psi_set(&bufpos, &psi->some) < 0)
        goto err_return;
      break;
    case 'f':
      // assume full
      if (mqtteer_psi_set(&bufpos, &psi->full) < 0)
        goto err_return;
      break;
    default:
      fprintf(stderr, "unknown pressure type %s\n", bufpos);
      goto err_return;
    }
  }

  cclose(psi_fd);
  return 0;

err_return:
  cclose(psi_fd);
  return -1;
}

void mqtteer_announce_topics(mqtteer_reports *reports) {
  if (mqtteer_debug)
    printf("announcing this device\n");

  mqtteer_send_discovery(RUNNING_ENTITY_NAME, NULL, NULL);

  for (int i = 0; i < reports->nb; i++) {
    mqtteer_report report = reports->reports[i];
    mqtteer_send_discovery(report.name, report.device_class,
                           report.unit_of_measurement);
  }
}

void mqtteer_send_metrics(mqtteer_reports *reports) {
  cJSON *state_obj = cJSON_CreateObject();
  char state_topic[mqtteer_state_topic_len()];
  mqtteer_get_state_topic_name(state_topic);

  cJSON_AddBoolToObject(state_obj, RUNNING_ENTITY_NAME, true);
  for (int i = 0; i < reports->nb; i++) {
    mqtteer_report report = reports->reports[i];
    switch (report.value_type) {
    case MQTTEER_TYPE_DOUBLE:
      cJSON_AddNumberToObject(state_obj, report.name, report.value->dblval);
      break;
    case MQTTEER_TYPE_LONG:
      cJSON_AddNumberToObject(state_obj, report.name, report.value->lval);
      break;
    case MQTTEER_TYPE_UNSIGNED_LONG:
      cJSON_AddNumberToObject(state_obj, report.name, report.value->ulval);
      break;
    case MQTTEER_TYPE_INT:
      cJSON_AddNumberToObject(state_obj, report.name, report.value->ival);
      break;
    case MQTTEER_TYPE_STR:
      cJSON_AddStringToObject(state_obj, report.name, report.value->strval);
      break;
    }
  }

  char *payload = cJSON_Print(state_obj);

  if (mqtteer_debug)
    printf("%s\n", payload);

  mqtteer_send(state_topic, payload);
  free(payload);
  cJSON_Delete(state_obj);
}

void mqtteer_loadavg_reports(mqtteer_reports *reports) {
  double av1, av5, av15;
  procps_loadavg(&av1, &av5, &av15);

  mqtteer_new_report_long(reports, "load1", av1, "power_factor", NULL);
  mqtteer_new_report_long(reports, "load5", av5, "power_factor", NULL);
  mqtteer_new_report_long(reports, "load15", av15, "power_factor", NULL);
}

void mqtteer_uptime_report(mqtteer_reports *reports) {
  double uptime;
  procps_uptime(&uptime, NULL);

  mqtteer_new_report_dbl(reports, "uptime", uptime, "duration", "s");
}

void mqtteer_meminfo_reports(mqtteer_reports *reports) {
  struct meminfo_info *meminfo = NULL;
  unsigned long used, total;

  if (procps_meminfo_new(&meminfo) < 0) {
    fprintf(stderr, "failed to get memory info");
    exit(EXIT_FAILURE);
  }

  used = MEMINFO_GET(meminfo, MEMINFO_MEM_USED, ul_int);
  total = MEMINFO_GET(meminfo, MEMINFO_MEM_TOTAL, ul_int);
  procps_meminfo_unref(&meminfo);

  mqtteer_new_report_ulong(reports, "used_memory", used, "data_size", "kB");
  mqtteer_new_report_ulong(reports, "total_memory", total, "data_size", "kB");
}

void mqtteer_psi_reports(mqtteer_reports *reports, const char *kind) {
  struct mqtteer_psi psi;
  int ret = mqtteer_psi_get(kind, &psi);
  if (ret < 0)
    return;

  char name[strlen("psi_memory_some_avg300") + 1]; // longest name

  sprintf(name, "psi_%s_some_avg10", kind);
  mqtteer_new_report_dbl(reports, name, psi.some.avg10, "power_factor", "%");
  sprintf(name, "psi_%s_some_avg60", kind);
  mqtteer_new_report_dbl(reports, name, psi.some.avg60, "power_factor", "%");
  sprintf(name, "psi_%s_some_avg300", kind);
  mqtteer_new_report_dbl(reports, name, psi.some.avg300, "power_factor", "%");
  sprintf(name, "psi_%s_some_total", kind);
  mqtteer_new_report_dbl(reports, name, psi.some.total, "power_factor", "μs");

  sprintf(name, "psi_%s_full_avg10", kind);
  mqtteer_new_report_dbl(reports, name, psi.full.avg10, "power_factor", "%");
  sprintf(name, "psi_%s_full_avg60", kind);
  mqtteer_new_report_dbl(reports, name, psi.full.avg60, "power_factor", "%");
  sprintf(name, "psi_%s_full_avg300", kind);
  mqtteer_new_report_dbl(reports, name, psi.full.avg300, "power_factor", "%");
  sprintf(name, "psi_%s_full_total", kind);
  mqtteer_new_report_dbl(reports, name, psi.full.total, "power_factor", "μs");
}

void mqtteer_sensors_reports(mqtteer_reports *reports) {
  int nr_chip = 0, nr_feat = 0;
  const struct sensors_chip_name *chip = NULL;
  struct mqtteer_sensor *sensor;

  mqtteer_sensors_init();

  while ((chip = sensors_get_detected_chips(NULL, &nr_chip)) != NULL) {
    while ((sensor = mqtteer_get_sensor(chip, &nr_feat)) != NULL) {
      mqtteer_new_report_dbl(reports, sensor->name, sensor->value, sensor->unit,
                             sensor->unit);
      mqtteer_sensor_free(sensor);
    }
  }
  sensors_cleanup();
}

void mqtteer_batteries_reports(mqtteer_reports *reports) {
  mqtteer_batteries *batteries = mqtteer_get_batteries();

  for (unsigned i = 0; i < batteries->n; i++) {
    mqtteer_new_report_int(reports, batteries->batteries[i].name,
                           batteries->batteries[i].capacity, "battery", "%");
  }

  mqtteer_free_batteries(batteries);
}

mqtteer_reports *mqtteer_get_reports() {
  mqtteer_reports *reports = malloc(sizeof(mqtteer_reports));
  reports->nb = 0;
  reports->reports = NULL;

  mqtteer_loadavg_reports(reports);
  mqtteer_uptime_report(reports);
  mqtteer_meminfo_reports(reports);

  mqtteer_sensors_reports(reports);
  mqtteer_batteries_reports(reports);

  for (unsigned i = 0; i < NPSI_KINDS; i++)
    mqtteer_psi_reports(reports, PRESSURE_KINDS[i]);

  return reports;
}

void mqtteer_set_will() {
  char payload[] = "{\"" RUNNING_ENTITY_NAME "\":false}";
  int payload_len = strlen(payload);

  char state_topic[mqtteer_state_topic_len()];
  mqtteer_get_state_topic_name(state_topic);

  mosquitto_will_set(mosq, state_topic, payload_len, payload, 0, false);
}

static void mqtteer_init_mosquitto() {
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
}

int main(void) {
  mqtteer_init_mosquitto();

  while (true) {
    mqtteer_reports *reports = mqtteer_get_reports();
    mqtteer_announce_topics(reports);
    mqtteer_send_metrics(reports);
    mqtteer_free_reports(reports);
    sleep(60);
  }

  exit(EXIT_SUCCESS);
}
