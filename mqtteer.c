
#include <cjson/cJSON.h>
#include <libproc2/meminfo.h>
#include <libproc2/misc.h>
#include <locale.h>
#include <mosquitto.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOSQ_KEEPALIVE 30
#define DISCOVERY_TOPIC_PREFIX "homeassistant"

static char *mqtteer_device_name;

void cleanup(struct mosquitto *mosq, int exit_code) {
  mosquitto_destroy(mosq);
  mosquitto_lib_cleanup();
  exit(exit_code);
}

void mqtteer_send(struct mosquitto *mosq, char *topic, char* payload) {
  int payload_len;
  int ret;

  payload_len = strlen(payload);
  ret = mosquitto_publish(mosq, NULL, topic, payload_len, payload, 0, false);
  if (ret != MOSQ_ERR_SUCCESS) {
    fprintf(stderr, "error %d", ret);
    cleanup(mosq, EXIT_FAILURE);
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
  return strlen(name) + strlen(mqtteer_device_name)
       + strlen(DISCOVERY_TOPIC_PREFIX "/sensor///config") + 1;
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

void mqtteer_send_discovery(struct mosquitto *mosq, char *name,
                            char *device_class, char *unit_of_measurement) {
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

  #define TEMPLATE_STR_FORMAT "{{ value_json.%s }}"
  char template_str[strlen(name) + strlen(TEMPLATE_STR_FORMAT) - 1];
  sprintf(template_str, TEMPLATE_STR_FORMAT, name);
  cJSON_AddStringToObject(discovery_obj, "value_template", template_str);

  if (device_class != NULL)
    cJSON_AddStringToObject(discovery_obj, "device_class", device_class);
  if (unit_of_measurement != NULL)
    cJSON_AddStringToObject(discovery_obj, "unit_of_measurement", unit_of_measurement);

  cJSON *device_obj = cJSON_CreateObject();
  cJSON_AddStringToObject(device_obj, "name", mqtteer_device_name);

  cJSON *identifiers_arr = cJSON_CreateArray();
  cJSON_AddItemToArray(identifiers_arr, cJSON_CreateString(mqtteer_device_name));

  cJSON_AddItemToObject(device_obj, "identifiers", identifiers_arr);
  cJSON_AddItemToObject(discovery_obj, "device", device_obj);

  char *discovery_payload = cJSON_Print(discovery_obj);
  mqtteer_send(mosq, discovery_topic, discovery_payload);

  free(discovery_payload);
  cJSON_Delete(discovery_obj);
}

char * mqtteer_getenv(char *name) {
  char *value = getenv(name);
  if (value == NULL) {
    fprintf(stderr, "%s environment variable is not set", name);
    exit(EXIT_FAILURE);
  }
  return value;
}

void mqtteer_announce_topics(struct mosquitto *mosq) {
    printf("announcing this device\n");

    mqtteer_send_discovery(mosq, "uptime", "duration", "s");
    mqtteer_send_discovery(mosq, "load1", "power_factor", NULL);
    mqtteer_send_discovery(mosq, "load5", "power_factor", NULL);
    mqtteer_send_discovery(mosq, "load15", "power_factor", NULL);
    mqtteer_send_discovery(mosq, "used_memory", "data_size", "kB");
    mqtteer_send_discovery(mosq, "total_memory", "data_size", "kB");
}

void mqtteer_report_metrics(struct mosquitto *mosq) {
  struct meminfo_info *meminfo = NULL;
  double uptime;
  double av1, av5, av15;
  unsigned long used, total;
  char state_topic[mqtteer_state_topic_len()];

  procps_loadavg(&av1, &av5, &av15);
  procps_uptime(&uptime, NULL);

  if (procps_meminfo_new(&meminfo) < 0) {
    fprintf(stderr, "failed to get memory info");
    cleanup(mosq, EXIT_FAILURE);
  }
  used = MEMINFO_GET(meminfo, MEMINFO_MEM_USED, ul_int);
  total = MEMINFO_GET(meminfo, MEMINFO_MEM_TOTAL, ul_int);

  cJSON *state_obj = cJSON_CreateObject();
  cJSON_AddNumberToObject(state_obj, "uptime", uptime);
  cJSON_AddNumberToObject(state_obj, "load1", av1);
  cJSON_AddNumberToObject(state_obj, "load5", av5);
  cJSON_AddNumberToObject(state_obj, "load15", av15);
  cJSON_AddNumberToObject(state_obj, "used_memory", used);
  cJSON_AddNumberToObject(state_obj, "total_memory", total);

  char *payload = cJSON_Print(state_obj);

  mqtteer_get_state_topic_name(state_topic);
  mqtteer_send(mosq, state_topic, payload);
  free(payload);
  cJSON_Delete(state_obj);
}

int main(int argc, char *argv[]) {
  struct mosquitto *mosq;
  int mosq_port;

  char *mosq_username = mqtteer_getenv("MQTTEER_USERNAME");
  char *mosq_password = mqtteer_getenv("MQTTEER_PASSWORD");
  char *mosq_host = mqtteer_getenv("MQTTEER_HOST");
  char *mosq_port_str = getenv("MQTTEER_PORT");
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
  mosq = mosquitto_new(NULL, true, NULL);
  mosquitto_username_pw_set(mosq, mosq_username, mosq_password);
  mosquitto_connect(mosq, mosq_host, mosq_port, MOSQ_KEEPALIVE);

  if (argc > 1 && strcmp(argv[1], "announce") == 0) {
    mqtteer_announce_topics(mosq);
  } else {
    mqtteer_report_metrics(mosq);
  }

  cleanup(mosq, EXIT_SUCCESS);
}
