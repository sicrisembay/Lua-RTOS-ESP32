#include "luartos.h"

#if CONFIG_LUA_RTOS_LUA_USE_AWS
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "modules.h"
#include "error.h"
#include "sys.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "aws_iot_config.h"
#include "aws_iot_log.h"
#include "aws_iot_version.h"
#include "aws_iot_mqtt_client_interface.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    AWS_IoT_Client client;
    IoT_Client_Init_Params mqttInitParams;
    IoT_Client_Connect_Params connectParams;
    IoT_Publish_Message_Params paramsQOS0;
    IoT_Publish_Message_Params paramsQOS1;
} aws_userdata_t;

static const char * TAG = "aws_iot";

static void disconnectCallbackHandler(AWS_IoT_Client *pClient, void *data) {
    ESP_LOGW(TAG, "MQTT Disconnect");
    IoT_Error_t rc = FAILURE;

    if(NULL == pClient) {
        return;
    }

    if(aws_iot_is_autoreconnect_enabled(pClient)) {
        ESP_LOGI(TAG, "Auto Reconnect is enabled, Reconnecting attempt will start now");
    } else {
        ESP_LOGW(TAG, "Auto Reconnect not enabled. Starting manual reconnect...");
        rc = aws_iot_mqtt_attempt_reconnect(pClient);
        if(NETWORK_RECONNECTED == rc) {
            ESP_LOGW(TAG, "Manual Reconnect Successful");
        } else {
            ESP_LOGW(TAG, "Manual Reconnect Failed - %d", rc);
        }
    }
}

static int laws_client(lua_State * L)
{
    IoT_Error_t rc = FAILURE;
    size_t lenClientId;
    size_t lenHost;

    const char * clientId = luaL_checklstring(L, 1, &lenClientId);
    const char * host = luaL_checklstring(L, 2, &lenHost);
    int port = luaL_checkinteger(L, 3);
    const char * ca_file = luaL_optstring(L, 4, NULL);
    const char * dev_ca_file = luaL_optstring(L, 5, NULL);
    const char * dev_key_file = luaL_optstring(L, 6, NULL);

    /* allocate user data */
    aws_userdata_t * aws = (aws_userdata_t *) lua_newuserdata(L, sizeof(aws_userdata_t));

    /* Initialize user data */
    printf("AWS IoT SDK Version %d.%d.%d-%s\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);
    aws->mqttInitParams = iotClientInitParamsDefault;
    aws->connectParams = iotClientConnectParamsDefault;

    aws->mqttInitParams.enableAutoReconnect = false; /* Enabled later */
    aws->mqttInitParams.pHostURL = strdup(host);
    aws->mqttInitParams.port = port;
    aws->mqttInitParams.pRootCALocation = (ca_file ? strdup(ca_file) : NULL);
    aws->mqttInitParams.pDeviceCertLocation = (dev_ca_file ? strdup(dev_ca_file) : NULL);
    aws->mqttInitParams.pDevicePrivateKeyLocation = (dev_key_file ? strdup(dev_key_file) : NULL);
    aws->mqttInitParams.mqttCommandTimeout_ms = 20000;
    aws->mqttInitParams.tlsHandshakeTimeout_ms = 5000;
    aws->mqttInitParams.isSSLHostnameVerify = true;
    aws->mqttInitParams.disconnectHandler = disconnectCallbackHandler;
    aws->mqttInitParams.disconnectHandlerData = NULL;
    rc = aws_iot_mqtt_init(&(aws->client), &(aws->mqttInitParams));
    if(SUCCESS != rc) {
        ESP_LOGE(TAG, "aws_iot_mqtt_init returned error : %d ", rc);
        abort();
    }

    aws->connectParams.keepAliveIntervalInSec = 10;
    aws->connectParams.isCleanSession = true;
    aws->connectParams.MQTTVersion = MQTT_3_1_1;
    aws->connectParams.pClientID = (clientId ? strdup(clientId) : NULL);
    aws->connectParams.clientIDLen = (uint16_t)lenClientId;
    aws->connectParams.isWillMsgPresent = false;

    /* User metatable */
    luaL_getmetatable(L, "aws.cli");
    lua_setmetatable(L, -2);

    return 1;
}

static int laws_connect(lua_State * L)
{
    IoT_Error_t rc = FAILURE;
    uint32_t retry = 0;

    /* Get user data */
    aws_userdata_t * aws = (aws_userdata_t *) luaL_checkudata(L, 1, "aws.cli");
    luaL_argcheck(L, aws, 1, "aws expected");

    printf("Hello from laws_connect\n");

    retry = 0;
    while(1) {
        rc = aws_iot_mqtt_connect(&(aws->client), &(aws->connectParams));
        if(SUCCESS != rc) {
            ESP_LOGE(TAG, "Error(%d) connecting to %s:%d", rc, aws->mqttInitParams.pHostURL, aws->mqttInitParams.port);
            vTaskDelay(1000 / portTICK_RATE_MS);
        }
        retry++;
        if(retry >= 10) {
            ESP_LOGE(TAG, "Connect Retry exceeded.");
            return 0;
        }
    }

    rc = aws_iot_mqtt_autoreconnect_set_status(&(aws->client), true);
    if(SUCCESS != rc) {
        ESP_LOGE(TAG, "Unable to set Auto Reconnect to true - %d", rc);
        return 0;
    }

    printf("Connected to AWS broker.");

    return 0;
}

static int laws_client_gc(lua_State * L)
{
    printf("Hello from laws_client_gc\n");
    aws_userdata_t * aws = (aws_userdata_t *)luaL_testudata(L, 1, "aws.cli");
    if(aws) {
        printf("Destroy and garbage collect user data\n");
        if(aws->mqttInitParams.pHostURL) {
            free((char *)aws->mqttInitParams.pHostURL);
        }
        if(aws->mqttInitParams.pRootCALocation) {
            free((char *)aws->mqttInitParams.pRootCALocation);
        }
        if(aws->mqttInitParams.pDeviceCertLocation) {
            free((char *)aws->mqttInitParams.pDeviceCertLocation);
        }
        if(aws->mqttInitParams.pDevicePrivateKeyLocation) {
            free((char *)aws->mqttInitParams.pDevicePrivateKeyLocation);
        }
    }
    return 0;
}

static const LUA_REG_TYPE laws_map[] = {
    { LSTRKEY("client"), LFUNCVAL(laws_client) },

    /* Sentinel */
    { LNILKEY, LNILVAL }
};


static const LUA_REG_TYPE laws_client_map[] = {
    { LSTRKEY("connect"), LFUNCVAL(laws_connect) },

    { LSTRKEY( "__metatable" ),   LROVAL  ( laws_client_map ) },
    { LSTRKEY( "__index"     ),   LROVAL  ( laws_client_map ) },
    { LSTRKEY( "__gc"        ),   LFUNCVAL( laws_client_gc  ) },
    /* Sentinel */
    { LNILKEY, LNILVAL}
};


LUALIB_API int luaopen_aws( lua_State *L ) {
    luaL_newmetarotable(L, "aws.cli", (void *)laws_client_map);
    printf("Hello from luaopen_aws\n");
    LNEWLIB(L, aws);
}

MODULE_REGISTER_ROM(AWS, aws, laws_map, luaopen_aws, 1);

#endif /* #if CONFIG_LUA_RTOS_LUA_USE_AWS_IOT */
