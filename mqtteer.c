
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

void mqtteer_send(struct mosquitto *mosq, char *topic, char* payload) {
  int payload_len;
  int ret;

  payload_len = strlen(payload);
  ret = mosquitto_publish(mosq, NULL, topic, payload_len, payload, 0, false);
  if (ret != MOSQ_ERR_SUCCESS)
    fprintf(stderr, "error %d", ret);
}

int mqtteer_state_topic_len(char *name, char* device_name) {
  return strlen(name) + strlen(device_name)
       + strlen(DISCOVERY_TOPIC_PREFIX "/sensor///state") + 1;
}

void mqtteer_get_state_topic_name(char *state_topic, char *name, char *device_name) {
  sprintf(state_topic, DISCOVERY_TOPIC_PREFIX "/sensor/%s/%s/state",
          device_name, name);
}

void mqtteer_send_ulong(struct mosquitto *mosq, char *name, char *device_name, unsigned long payload_data) {
  cJSON *json_payload = cJSON_CreateObject();
  cJSON_AddNumberToObject(json_payload, "new_value", payload_data);

  char topic[mqtteer_state_topic_len(name, device_name)];
  mqtteer_get_state_topic_name(topic, name, device_name);

  char *payload = cJSON_Print(json_payload);
  mqtteer_send(mosq, topic, payload);
}

void mqtteer_send_dbl(struct mosquitto *mosq, char *name, char *device_name, double payload_data) {
  cJSON *json_payload = cJSON_CreateObject();
  cJSON_AddNumberToObject(json_payload, "new_value", payload_data);

  char topic[mqtteer_state_topic_len(name, device_name)];
  mqtteer_get_state_topic_name(topic, name, device_name);

  char *payload = cJSON_Print(json_payload);
  mqtteer_send(mosq, topic, payload);
}

int mqtteer_discovery_topic_len(char *name, char* device_name) {
  return strlen(name) + strlen(device_name)
       + strlen(DISCOVERY_TOPIC_PREFIX "/sensor///config") + 1;
}

void mqtteer_get_discovery_topic_name(char *state_topic, char *name, char *device_name) {
  sprintf(state_topic, DISCOVERY_TOPIC_PREFIX "/sensor/%s/%s/config",
          device_name, name);
}

int mqtteer_unique_id_len(char *name, char *device_name) {
  return strlen(name) + strlen(device_name) + 2;
}

void mqtteer_get_unique_id(char *unique_id, char *name, char *device_name) {
  sprintf(unique_id, "%s_%s", device_name, name);
}

void mqtteer_send_discovery(struct mosquitto *mosq, char *name,
                            char *device_name, char *device_class) {
  char unique_id[mqtteer_unique_id_len(name, device_name)];
  mqtteer_get_unique_id(unique_id, name, device_name);
  char state_topic[mqtteer_state_topic_len(name, device_name)];
  mqtteer_get_state_topic_name(state_topic, name, device_name);

  char discovery_topic[mqtteer_discovery_topic_len(name, device_name)];
  mqtteer_get_discovery_topic_name(discovery_topic, name, device_name);

  cJSON *discovery_obj = cJSON_CreateObject();

  cJSON_AddStringToObject(discovery_obj, "name", name);
  cJSON_AddStringToObject(discovery_obj, "state_topic", state_topic);
  cJSON_AddStringToObject(discovery_obj, "unique_id", unique_id);
  cJSON_AddStringToObject(discovery_obj, "value_template", "{{ value_json.new_value }}");
  if (device_class != NULL)
    cJSON_AddStringToObject(discovery_obj, "device_class", device_class);
  
  cJSON *device_obj = cJSON_CreateObject();
  cJSON_AddStringToObject(device_obj, "name", device_name);

  cJSON *identifiers_arr = cJSON_CreateArray();
  cJSON_AddItemToArray(identifiers_arr, cJSON_CreateString(device_name));

  cJSON_AddItemToObject(device_obj, "identifiers", identifiers_arr);
  cJSON_AddItemToObject(discovery_obj, "device", device_obj);

  char *discovery_payload = cJSON_Print(discovery_obj);
  mqtteer_send(mosq, discovery_topic, discovery_payload);

  free(discovery_payload);
}

char * mqtteer_getenv(char *name) {
  char *value = getenv(name);
  if (value == NULL) {
    printf("%s environment variable is not set", name);
    exit(EXIT_FAILURE);
  }
  return value;
}

int main(int argc, char *argv[]) {
  struct mosquitto *mosq;
  struct meminfo_info *meminfo;
  int mosq_port;
  double uptime;
  double av1, av5, av15;
  unsigned long used, total;

  char *mosq_username = mqtteer_getenv("MOSQ_USERNAME");
  char *mosq_password = mqtteer_getenv("MOSQ_PASSWORD");
  char *mosq_host = mqtteer_getenv("MOSQ_HOST");
  char *mosq_port_str = getenv("MOSQ_PORT");

  if (mosq_port_str == NULL)
    mosq_port = 1883;
  else
    mosq_port = atoi(mosq_port_str);

  char *mqtteer_device_name = mqtteer_getenv("MQTTEER_DEVICE_NAME");

  mosquitto_lib_init();
  mosq = mosquitto_new(NULL, true, NULL);
  mosquitto_username_pw_set(mosq, mosq_username, mosq_password);
  mosquitto_connect(mosq, mosq_host, mosq_port, MOSQ_KEEPALIVE);

  mqtteer_send_discovery(mosq, "uptime", mqtteer_device_name, "duration");
  mqtteer_send_discovery(mosq, "load1", mqtteer_device_name, "power_factor");
  mqtteer_send_discovery(mosq, "load5", mqtteer_device_name, "power_factor");
  mqtteer_send_discovery(mosq, "load15", mqtteer_device_name, "power_factor");
  mqtteer_send_discovery(mosq, "used_memory", mqtteer_device_name, "data_size");
  mqtteer_send_discovery(mosq, "total_memory", mqtteer_device_name, "data_size");

  procps_loadavg(&av1, &av5, &av15);
  procps_uptime(&uptime, NULL);
  procps_meminfo_new(&meminfo);
  used = MEMINFO_GET(meminfo, MEMINFO_MEM_USED, ul_int);
  total = MEMINFO_GET(meminfo, MEMINFO_MEM_TOTAL, ul_int);

  mqtteer_send_dbl(mosq, "uptime", mqtteer_device_name, uptime);
  mqtteer_send_dbl(mosq, "load1", mqtteer_device_name, av1);
  mqtteer_send_dbl(mosq, "load5", mqtteer_device_name, av5);
  mqtteer_send_dbl(mosq, "load15", mqtteer_device_name, av15);
  mqtteer_send_ulong(mosq, "used_memory", mqtteer_device_name, used);
  mqtteer_send_ulong(mosq, "total_memory", mqtteer_device_name, total);

  mosquitto_destroy(mosq);
  mosquitto_lib_cleanup();
  return EXIT_SUCCESS;
}
