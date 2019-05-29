// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "azure_c_shared_utility/optimize_size.h"
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/string_tokenizer.h"
#include "azure_c_shared_utility/doublylinkedlist.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/tickcounter.h"
#include "azure_c_shared_utility/constbuffer.h"
#include "azure_c_shared_utility/platform.h"
#include "azure_c_shared_utility/singlylinkedlist.h"
#include "azure_c_shared_utility/shared_util_options.h"
#include "azure_c_shared_utility/agenttime.h"

#include "iothub_client_core_ll.h"
#include "iothub_client_options.h"
#include "iothub_client_version.h"
#include "iothub_transport_ll.h"
#include "internal/iothub_client_authorization.h"
#include "internal/iothub_client_private.h"
#include "internal/iothub_client_diagnostic.h"
#include "internal/iothubtransport.h"

#include "iothub_prov_transport.h"

#ifndef DONT_USE_UPLOADTOBLOB
#include "internal/iothub_client_ll_uploadtoblob.h"
#endif

#ifdef USE_EDGE_MODULES
#include "azure_c_shared_utility/envvariable.h"
#include "azure_prov_client/iothub_security_factory.h"
#include "internal/iothub_client_edge.h"
#endif

#define LOG_ERROR_RESULT LogError("result = %s", MU_ENUM_TO_STRING(IOTHUB_CLIENT_RESULT, result));
#define INDEFINITE_TIME ((time_t)(-1))

MU_DEFINE_ENUM_STRINGS(IOTHUB_CLIENT_FILE_UPLOAD_RESULT, IOTHUB_CLIENT_FILE_UPLOAD_RESULT_VALUES);
MU_DEFINE_ENUM_STRINGS(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_RESULT_VALUES);
MU_DEFINE_ENUM_STRINGS(IOTHUB_CLIENT_RETRY_POLICY, IOTHUB_CLIENT_RETRY_POLICY_VALUES);
MU_DEFINE_ENUM_STRINGS(IOTHUB_CLIENT_STATUS, IOTHUB_CLIENT_STATUS_VALUES);
MU_DEFINE_ENUM_STRINGS(IOTHUB_IDENTITY_TYPE, IOTHUB_IDENTITY_TYPE_VALUE);
MU_DEFINE_ENUM_STRINGS(IOTHUB_PROCESS_ITEM_RESULT, IOTHUB_PROCESS_ITEM_RESULT_VALUE);
MU_DEFINE_ENUM_STRINGS(IOTHUB_CLIENT_IOTHUB_METHOD_STATUS, IOTHUB_CLIENT_IOTHUB_METHOD_STATUS_VALUES);
MU_DEFINE_ENUM_STRINGS(IOTHUB_CLIENT_CONFIRMATION_RESULT, IOTHUB_CLIENT_CONFIRMATION_RESULT_VALUES);
MU_DEFINE_ENUM_STRINGS(IOTHUB_CLIENT_CONNECTION_STATUS, IOTHUB_CLIENT_CONNECTION_STATUS_VALUES);
MU_DEFINE_ENUM_STRINGS(IOTHUB_CLIENT_CONNECTION_STATUS_REASON, IOTHUB_CLIENT_CONNECTION_STATUS_REASON_VALUES);
MU_DEFINE_ENUM_STRINGS(TRANSPORT_TYPE, TRANSPORT_TYPE_VALUES);
MU_DEFINE_ENUM_STRINGS(DEVICE_TWIN_UPDATE_STATE, DEVICE_TWIN_UPDATE_STATE_VALUES);

#ifdef USE_PROV_MODULE
MU_DEFINE_ENUM_STRINGS(PROV_DEVICE_RESULT, PROV_DEVICE_RESULT_VALUE);
MU_DEFINE_ENUM_STRINGS(PROV_DEVICE_REG_STATUS, PROV_DEVICE_REG_STATUS_VALUES);
#endif // #ifdef USE_PROV_MODULE

#ifndef DONT_USE_UPLOADTOBLOB
MU_DEFINE_ENUM_STRINGS(IOTHUB_CLIENT_FILE_UPLOAD_GET_DATA_RESULT, IOTHUB_CLIENT_FILE_UPLOAD_GET_DATA_RESULT_VALUES);
#endif // DONT_USE_UPLOADTOBLOB

#define CALLBACK_TYPE_VALUES \
    CALLBACK_TYPE_NONE,      \
    CALLBACK_TYPE_SYNC,    \
    CALLBACK_TYPE_ASYNC

MU_DEFINE_ENUM(CALLBACK_TYPE, CALLBACK_TYPE_VALUES)
MU_DEFINE_ENUM_STRINGS(CALLBACK_TYPE, CALLBACK_TYPE_VALUES)

typedef enum IOTHUB_OPERATION_INITIALIZE_TAG
{
    IOTHUB_OP_C2D_SUB           = 0x00000001,
    IOTHUB_OP_MSG_DISPOSITION   = 0x00000002,
    IOTHUB_OP_DEVICE_TWIN_SUB   = 0x00000004,
    IOTHUB_OP_METHOD_SUB        = 0x00000008,
} IOTHUB_OPERATION_INITIALIZE;

typedef enum IOTHUB_REG_STATE_TAG
{
    OP_STATE_IDLE,
    OP_STATE_PRE_REGISTER,
    OP_STATE_REGISTERING,
    OP_STATE_REGISTERED,
    OP_STATE_IOT_STAGE,
    OP_STATE_ERROR
} IOTHUB_REG_STATE;

typedef enum CLIENT_CONFIG_TYPE_TAG
{
    CONFIG_TYPE_DEVICE,
    CONFIG_TYPE_CLIENT,
    CONFIG_TYPE_DEVICE_AUTH,
} CLIENT_CONFIG_TYPE;

typedef struct IOTHUB_METHOD_CALLBACK_DATA_TAG
{
    CALLBACK_TYPE type;
    IOTHUB_CLIENT_DEVICE_METHOD_CALLBACK_ASYNC callbackSync;
    IOTHUB_CLIENT_INBOUND_DEVICE_METHOD_CALLBACK callbackAsync;
    void* userContextCallback;
}IOTHUB_METHOD_CALLBACK_DATA;

typedef struct IOTHUB_EVENT_CALLBACK_TAG
{
    STRING_HANDLE inputName;
    IOTHUB_CLIENT_MESSAGE_CALLBACK_ASYNC callbackAsync;
    IOTHUB_CLIENT_MESSAGE_CALLBACK_ASYNC_EX callbackAsyncEx;
    void* userContextCallback;
    void* userContextCallbackEx;
}IOTHUB_EVENT_CALLBACK;

typedef struct IOTHUB_MESSAGE_CALLBACK_DATA_TAG
{
    CALLBACK_TYPE type;
    IOTHUB_CLIENT_MESSAGE_CALLBACK_ASYNC callbackSync;
    IOTHUB_CLIENT_MESSAGE_CALLBACK_ASYNC_EX callbackAsync;
    void* userContextCallback;
}IOTHUB_MESSAGE_CALLBACK_DATA;

typedef struct GET_TWIN_CONTEXT_TAG
{
    IOTHUB_CLIENT_DEVICE_TWIN_CALLBACK callback;
    void* context;
} GET_TWIN_CONTEXT;

typedef struct IOTHUB_CLIENT_CORE_LL_HANDLE_DATA_TAG
{
    DLIST_ENTRY waitingToSend;
    DLIST_ENTRY iot_msg_queue;
    DLIST_ENTRY iot_ack_queue;
    TRANSPORT_LL_HANDLE transportHandle;
    bool isSharedTransport;
    IOTHUB_DEVICE_HANDLE deviceHandle;
    TRANSPORT_PROVIDER_FIELDS;
    IOTHUB_MESSAGE_CALLBACK_DATA messageCallback;
    IOTHUB_METHOD_CALLBACK_DATA methodCallback;
    IOTHUB_CLIENT_CONNECTION_STATUS_CALLBACK conStatusCallback;
    void* conStatusUserContextCallback;
    time_t lastMessageReceiveTime;
    TICK_COUNTER_HANDLE tickCounter; /*shared tickcounter used to track message timeouts in waitingToSend list*/
    tickcounter_ms_t currentMessageTimeout;
    uint64_t current_device_twin_timeout;
    IOTHUB_CLIENT_DEVICE_TWIN_CALLBACK deviceTwinCallback;
    void* deviceTwinContextCallback;
    IOTHUB_CLIENT_RETRY_POLICY retryPolicy;
    size_t retryTimeoutLimitInSeconds;
#ifndef DONT_USE_UPLOADTOBLOB
    IOTHUB_CLIENT_LL_UPLOADTOBLOB_HANDLE uploadToBlobHandle;
#endif
#ifdef USE_EDGE_MODULES
    IOTHUB_CLIENT_EDGE_HANDLE methodHandle;
#endif
    uint32_t data_msg_id;
    bool complete_twin_update_encountered;
    IOTHUB_AUTHORIZATION_HANDLE authorization_module;
    STRING_HANDLE product_info;
    IOTHUB_DIAGNOSTIC_SETTING_DATA diagnostic_setting;
    SINGLYLINKEDLIST_HANDLE event_callbacks;  // List of IOTHUB_EVENT_CALLBACK's
    bool log_trace;
    IOTHUB_REG_STATE registration_state;
    uint32_t iothub_op_init;
#ifdef USE_PROV_MODULE
    PROV_DEVICE_LL_HANDLE prov_handle;
    IOTHUB_CLIENT_TRANSPORT_PROVIDER iothub_provider;
#endif
}IOTHUB_CLIENT_CORE_LL_HANDLE_DATA;

typedef struct IOTHUB_CONFIG_NORMALIZED_TAG
{
    CLIENT_CONFIG_TYPE config_type;
    union IOTHUB_CONFIG_ITEMS_TAG
    {
        const IOTHUB_CLIENT_CONFIG* client_config;
        const IOTHUB_CLIENT_DEVICE_CONFIG* device_config;
    } IOTHUB_CONFIG_ITEMS;
} IOTHUB_CONFIG_NORMALIZED;

static const char HOSTNAME_TOKEN[] = "HostName";
static const char DEVICEID_TOKEN[] = "DeviceId";
static const char X509_TOKEN[] = "x509";
static const char X509_TOKEN_ONLY_ACCEPTABLE_VALUE[] = "true";
static const char DEVICEKEY_TOKEN[] = "SharedAccessKey";
static const char DEVICESAS_TOKEN[] = "SharedAccessSignature";
static const char PROTOCOL_GATEWAY_HOST_TOKEN[] = "GatewayHostName";
static const char MODULE_ID_TOKEN[] = "ModuleId";
static const char PROVISIONING_TOKEN[] = "UseProvisioning";
static const char PROVISIONING_ACCEPTABLE_VALUE[] = "true";


#ifdef USE_EDGE_MODULES
/*The following section should be moved to iothub_module_client_ll.c during impending refactor*/

static const char* ENVIRONMENT_VAR_EDGEHUB_CONNECTIONSTRING = "EdgeHubConnectionString";
static const char* ENVIRONMENT_VAR_EDGEHUB_CACERTIFICATEFILE = "EdgeModuleCACertificateFile";
static const char* ENVIRONMENT_VAR_EDGEAUTHSCHEME = "IOTEDGE_AUTHSCHEME";
static const char* ENVIRONMENT_VAR_EDGEDEVICEID = "IOTEDGE_DEVICEID";
static const char* ENVIRONMENT_VAR_EDGEMODULEID = "IOTEDGE_MODULEID";
static const char* ENVIRONMENT_VAR_EDGEHUBHOSTNAME = "IOTEDGE_IOTHUBHOSTNAME";
static const char* ENVIRONMENT_VAR_EDGEGATEWAYHOST = "IOTEDGE_GATEWAYHOSTNAME";
static const char* SAS_TOKEN_AUTH = "sasToken";


typedef struct EDGE_ENVIRONMENT_VARIABLES_TAG
{
    const char* connection_string;
    const char* ca_trusted_certificate_file;
    const char* auth_scheme;
    const char* device_id;
    const char* iothub_name;
    const char* iothub_suffix;
    const char* gatewayhostname;
    const char* module_id;
    char* iothub_buffer;
} EDGE_ENVIRONMENT_VARIABLES;


static int retrieve_edge_environment_variabes(EDGE_ENVIRONMENT_VARIABLES *edge_environment_variables)
{
    int result;
    const char* edgehubhostname;
    char* edgehubhostname_separator;

    if ((edge_environment_variables->connection_string = environment_get_variable(ENVIRONMENT_VAR_EDGEHUB_CONNECTIONSTRING)) != NULL)
    {
        if ((edge_environment_variables->ca_trusted_certificate_file = environment_get_variable(ENVIRONMENT_VAR_EDGEHUB_CACERTIFICATEFILE)) == NULL)
        {
            LogError("Environment variable %s is missing.  When %s is set, it is required", ENVIRONMENT_VAR_EDGEHUB_CACERTIFICATEFILE, ENVIRONMENT_VAR_EDGEHUB_CONNECTIONSTRING);
            result = MU_FAILURE;
        }
        else
        {
            // If we can read in the connection string and trusted certs, we're done.
            result = 0;
        }
    }
    else
    {
        // We're NOT using pre-configured EdgeConnection string / certificates.  In this case, we use these environment variables when
        // communicating to Edge service.
        if ((edge_environment_variables->auth_scheme = environment_get_variable(ENVIRONMENT_VAR_EDGEAUTHSCHEME)) == NULL)
        {
            LogError("Environment %s not set", ENVIRONMENT_VAR_EDGEAUTHSCHEME);
            result = MU_FAILURE;
        }
        else if (strcmp(edge_environment_variables->auth_scheme, SAS_TOKEN_AUTH) != 0)
        {
            LogError("Environment %s was set to %s, but only support for %s", ENVIRONMENT_VAR_EDGEAUTHSCHEME, edge_environment_variables->auth_scheme, SAS_TOKEN_AUTH);
            result = MU_FAILURE;
        }
        else if ((edge_environment_variables->device_id = environment_get_variable(ENVIRONMENT_VAR_EDGEDEVICEID)) == NULL)
        {
            LogError("Environment %s not set", ENVIRONMENT_VAR_EDGEDEVICEID);
            result = MU_FAILURE;
        }
        else if ((edgehubhostname = environment_get_variable(ENVIRONMENT_VAR_EDGEHUBHOSTNAME)) == NULL)
        {
            LogError("Environment %s not set", ENVIRONMENT_VAR_EDGEHUBHOSTNAME);
            result = MU_FAILURE;
        }
        else if ((edge_environment_variables->gatewayhostname = environment_get_variable(ENVIRONMENT_VAR_EDGEGATEWAYHOST)) == NULL)
        {
            LogError("Environment %s not set", ENVIRONMENT_VAR_EDGEGATEWAYHOST);
            result = MU_FAILURE;
        }
        else if ((edge_environment_variables->module_id = environment_get_variable(ENVIRONMENT_VAR_EDGEMODULEID)) == NULL)
        {
            LogError("Environment %s not set", ENVIRONMENT_VAR_EDGEMODULEID);
            result = MU_FAILURE;
        }
        // Make a copy of just ENVIRONMENT_VAR_EDGEHUBHOSTNAME.  We need to make changes in place (namely inserting a '\0')
        // and can't do this with system environment variable safely.
        else if (mallocAndStrcpy_s(&edge_environment_variables->iothub_buffer, edgehubhostname) != 0)
        {
            LogError("Unable to copy buffer");
            result = MU_FAILURE;
        }
        else if ((edgehubhostname_separator = strchr(edge_environment_variables->iothub_buffer, '.')) == NULL)
        {
            LogError("Environment edgehub %s invalid, requires '.' separator", edge_environment_variables->iothub_buffer);
            result = MU_FAILURE;
        }
        else if (*(edgehubhostname_separator + 1) == 0)
        {
            LogError("Environment edgehub %s invalid, no content after '.' separator", edge_environment_variables->iothub_buffer);
            result = MU_FAILURE;
        }
        else
        {
            edge_environment_variables->iothub_name = edge_environment_variables->iothub_buffer;
            *edgehubhostname_separator = 0;
            edge_environment_variables->iothub_suffix = edgehubhostname_separator + 1;
            result = 0;
        }
    }

    return result;
}

IOTHUB_CLIENT_EDGE_HANDLE IoTHubClientCore_LL_GetEdgeHandle(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle)
{
    IOTHUB_CLIENT_EDGE_HANDLE result;
    if (iotHubClientHandle != NULL)
    {
        result = iotHubClientHandle->methodHandle;
    }
    else
    {
        result = NULL;
    }

    return result;
}
#endif /* USE_EDGE_MODULES */

static void setTransportProtocol(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData, TRANSPORT_PROVIDER* protocol)
{
    handleData->IoTHubTransport_SendMessageDisposition = protocol->IoTHubTransport_SendMessageDisposition;
    handleData->IoTHubTransport_GetHostname = protocol->IoTHubTransport_GetHostname;
    handleData->IoTHubTransport_SetOption = protocol->IoTHubTransport_SetOption;
    handleData->IoTHubTransport_Create = protocol->IoTHubTransport_Create;
    handleData->IoTHubTransport_Destroy = protocol->IoTHubTransport_Destroy;
    handleData->IoTHubTransport_Register = protocol->IoTHubTransport_Register;
    handleData->IoTHubTransport_Unregister = protocol->IoTHubTransport_Unregister;
    handleData->IoTHubTransport_Subscribe = protocol->IoTHubTransport_Subscribe;
    handleData->IoTHubTransport_Unsubscribe = protocol->IoTHubTransport_Unsubscribe;
    handleData->IoTHubTransport_DoWork = protocol->IoTHubTransport_DoWork;
    handleData->IoTHubTransport_SetRetryPolicy = protocol->IoTHubTransport_SetRetryPolicy;
    handleData->IoTHubTransport_GetSendStatus = protocol->IoTHubTransport_GetSendStatus;
    handleData->IoTHubTransport_ProcessItem = protocol->IoTHubTransport_ProcessItem;
    handleData->IoTHubTransport_Subscribe_DeviceTwin = protocol->IoTHubTransport_Subscribe_DeviceTwin;
    handleData->IoTHubTransport_Unsubscribe_DeviceTwin = protocol->IoTHubTransport_Unsubscribe_DeviceTwin;
    handleData->IoTHubTransport_GetTwinAsync = protocol->IoTHubTransport_GetTwinAsync;
    handleData->IoTHubTransport_Subscribe_DeviceMethod = protocol->IoTHubTransport_Subscribe_DeviceMethod;
    handleData->IoTHubTransport_Unsubscribe_DeviceMethod = protocol->IoTHubTransport_Unsubscribe_DeviceMethod;
    handleData->IoTHubTransport_DeviceMethod_Response = protocol->IoTHubTransport_DeviceMethod_Response;
    handleData->IoTHubTransport_Subscribe_InputQueue = protocol->IoTHubTransport_Subscribe_InputQueue;
    handleData->IoTHubTransport_Unsubscribe_InputQueue = protocol->IoTHubTransport_Unsubscribe_InputQueue;
    handleData->IoTHubTransport_SetCallbackContext = protocol->IoTHubTransport_SetCallbackContext;
    handleData->IoTHubTransport_GetSupportedPlatformInfo = protocol->IoTHubTransport_GetSupportedPlatformInfo;
}

static bool is_event_equal(IOTHUB_EVENT_CALLBACK *event_callback, const char *input_name)
{
    bool result;

    if (event_callback != NULL)
    {
        const char* event_input_name = STRING_c_str(event_callback->inputName);
        if ((event_input_name != NULL) && (input_name != NULL))
        {
            // Matched the input queue name of a named handler
            result = (strcmp(event_input_name, input_name) == 0);
        }
        else if ((input_name == NULL) && (event_input_name == NULL))
        {
            // Matched the default handler
            result = true;
        }
        else
        {
            result = false;
        }
    }
    else
    {
        result = false;
    }
    return result;
}

static bool is_event_equal_for_match(LIST_ITEM_HANDLE list_item, const void* match_context)
{
    return is_event_equal((IOTHUB_EVENT_CALLBACK*)singlylinkedlist_item_get_value(list_item), (const char*)match_context);
}

static void device_twin_data_destroy(IOTHUB_DEVICE_TWIN* client_item)
{
    CONSTBUFFER_DecRef(client_item->report_data_handle);
    free(client_item);
}

static int create_edge_handle(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handle_data, const IOTHUB_CLIENT_CONFIG* config, const char* module_id)
{
    int result;
    (void)config;
    (void)module_id;
#ifdef USE_EDGE_MODULES
    /* There is no way to currently distinguish a regular module from a edge module, so this handle is created regardless of if appropriate.
    However, as a gateway hostname is required in order to create an Edge Handle, we need to at least make sure that exists
    in order to prevent errors.

    The end result is that all edge modules will have an EdgeHandle, but only some non-edge modules will have it.
    Regardless, non-edge modules will never be able to use the handle.
    */
    if (config->protocolGatewayHostName != NULL)
    {
        handle_data->methodHandle = IoTHubClient_EdgeHandle_Create(config, handle_data->authorization_module, module_id);

        if (handle_data->methodHandle == NULL)
        {
            LogError("Unable to IoTHubModuleClient_LL_MethodHandle_Create");
            result = MU_FAILURE;
        }
        else
        {
            result = 0;
        }
    }
    else
    {
        result = 0;
    }

#else
    (void)handle_data;
    result = 0;
#endif
    return result;
}

static int create_blob_upload_module(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handle_data)
{
    int result;
    (void)handle_data;
#ifndef DONT_USE_UPLOADTOBLOB
    STRING_HANDLE hostname;
    if ((hostname = handle_data->IoTHubTransport_GetHostname(handle_data->transportHandle)) == NULL)
    {
        LogError("Failure retrieving hostname");
        result = MU_FAILURE;
    }
    else
    {
        if ((handle_data->uploadToBlobHandle = IoTHubClient_LL_UploadToBlob_Create(STRING_c_str(hostname), handle_data->authorization_module) ) == NULL)
        {
            LogError("unable to IoTHubClientCore_LL_UploadToBlob_Create");
            result = MU_FAILURE;
        }
        else
        {
            result = 0;
        }
        STRING_delete(hostname);
    }
#else
    result = 0;
#endif
    return result;
}

static void destroy_blob_upload_module(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handle_data)
{
    (void)handle_data;
#ifndef DONT_USE_UPLOADTOBLOB
    /*Codes_SRS_IOTHUBCLIENT_LL_02_046: [ If creating the TICK_COUNTER_HANDLE fails then IoTHubClientCore_LL_Create shall fail and return NULL. ]*/
    IoTHubClient_LL_UploadToBlob_Destroy(handle_data->uploadToBlobHandle);
#endif
}

static void destroy_module_method_module(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handle_data)
{
    (void)handle_data;
#ifdef USE_EDGE_MODULES
    IoTHubClient_EdgeHandle_Destroy(handle_data->methodHandle);
#endif
}

static bool invoke_message_callback(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData, MESSAGE_CALLBACK_INFO* messageData)
{
    bool result;
    /* Codes_SRS_IOTHUBCLIENT_LL_09_004: [IoTHubClient_LL_GetLastMessageReceiveTime shall return lastMessageReceiveTime in localtime] */
    handleData->lastMessageReceiveTime = get_time(NULL);

    switch (handleData->messageCallback.type)
    {
        case CALLBACK_TYPE_NONE:
        {
            /*Codes_SRS_IOTHUBCLIENT_LL_02_032: [If the client is not subscribed to receive messages then IoTHubClient_LL_MessageCallback shall return false.] */
            LogError("Invalid workflow - not currently set up to accept messages");
            result = false;
            break;
        }
        case CALLBACK_TYPE_SYNC:
        {
            /*Codes_SRS_IOTHUBCLIENT_LL_02_030: [If messageCallbackType is LEGACY then IoTHubClient_LL_MessageCallback shall invoke the last callback function (the parameter messageCallback to IoTHubClient_LL_SetMessageCallback) passing the message and the passed userContextCallback.]*/
            IOTHUBMESSAGE_DISPOSITION_RESULT cb_result = handleData->messageCallback.callbackSync(messageData->messageHandle, handleData->messageCallback.userContextCallback);

            /*Codes_SRS_IOTHUBCLIENT_LL_10_007: [If messageCallbackType is LEGACY then IoTHubClient_LL_MessageCallback shall send the message disposition as returned by the client to the underlying layer.] */
            if (handleData->IoTHubTransport_SendMessageDisposition(messageData, cb_result) != IOTHUB_CLIENT_OK)
            {
                LogError("IoTHubTransport_SendMessageDisposition failed");
            }
            result = true;
            break;
        }
        case CALLBACK_TYPE_ASYNC:
        {
            /* Codes_SRS_IOTHUBCLIENT_LL_10_009: [If messageCallbackType is ASYNC then IoTHubClient_LL_MessageCallback shall return what messageCallbacEx returns.] */
            result = handleData->messageCallback.callbackAsync(messageData, handleData->messageCallback.userContextCallback);
            if (!result)
            {
                LogError("messageCallbackEx failed");
            }
            break;
        }
        default:
        {
            LogError("Invalid state");
            result = false;
            break;
        }
    }

    return result;
}

/*Codes_SRS_IOTHUBCLIENT_LL_10_032: ["product_info" - takes a char string as an argument to specify the product information(e.g. `"ProductName/ProductVersion"`). ]*/
/*Codes_SRS_IOTHUBCLIENT_LL_10_034: ["product_info" - shall store the given string concatenated with the sdk information and the platform information in the form(ProductInfo DeviceSDKName / DeviceSDKVersion(OSName OSVersion; Architecture). ]*/
static STRING_HANDLE make_product_info(const char* product, PLATFORM_INFO_OPTION option)
{
    STRING_HANDLE result;
    STRING_HANDLE pfi = platform_get_platform_info(option);
    if (pfi == NULL)
    {
        LogError("Platform get info failed");
        result = NULL;
    }
    else
    {
        if (product == NULL)
        {
            result = STRING_construct_sprintf("%s %s", CLIENT_DEVICE_TYPE_PREFIX CLIENT_DEVICE_BACKSLASH IOTHUB_SDK_VERSION, STRING_c_str(pfi));
        }
        else
        {
            result = STRING_construct_sprintf("%s %s %s", product, CLIENT_DEVICE_TYPE_PREFIX CLIENT_DEVICE_BACKSLASH IOTHUB_SDK_VERSION, STRING_c_str(pfi));
        }
        STRING_delete(pfi);
    }
    return result;
}

static void IoTHubClientCore_LL_SendComplete(PDLIST_ENTRY completed, IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* ctx)
{
    /*Codes_SRS_IOTHUBCLIENT_LL_02_022: [If parameter completed is NULL, or parameter handle is NULL then IoTHubClientCore_LL_SendBatch shall return.]*/
    if (
        (ctx == NULL) ||
        (completed == NULL)
        )
    {
        /*"shall return"*/
        LogError("invalid arg");
    }
    else
    {
        /*Codes_SRS_IOTHUBCLIENT_LL_02_027: [If parameter result is IOTHUB_CLIENT_CONFIRMATION_ERROR then IoTHubClientCore_LL_SendComplete shall call all the non-NULL callbacks with the result parameter set to IOTHUB_CLIENT_CONFIRMATION_ERROR and the context set to the context passed originally in the SendEventAsync call.] */
        /*Codes_SRS_IOTHUBCLIENT_LL_02_025: [If parameter result is IOTHUB_CLIENT_CONFIRMATION_OK then IoTHubClientCore_LL_SendComplete shall call all the non-NULL callbacks with the result parameter set to IOTHUB_CLIENT_CONFIRMATION_OK and the context set to the context passed originally in the SendEventAsync call.]*/
        PDLIST_ENTRY oldest;
        while ((oldest = DList_RemoveHeadList(completed)) != completed)
        {
            IOTHUB_MESSAGE_LIST* messageList = (IOTHUB_MESSAGE_LIST*)containingRecord(oldest, IOTHUB_MESSAGE_LIST, entry);
            /*Codes_SRS_IOTHUBCLIENT_LL_02_026: [If any callback is NULL then there shall not be a callback call.]*/
            if (messageList->callback != NULL)
            {
                messageList->callback(result, messageList->context);
            }
            IoTHubMessage_Destroy(messageList->messageHandle);
            free(messageList);
        }
    }
}

static void IoTHubClientCore_LL_RetrievePropertyComplete(DEVICE_TWIN_UPDATE_STATE update_state, const unsigned char* payLoad, size_t size, void* ctx)
{
    if (ctx == NULL)
    {
        /* Codes_SRS_IOTHUBCLIENT_LL_07_013: [ If handle is NULL then IoTHubClientCore_LL_RetrievePropertyComplete shall do nothing.] */
        LogError("Invalid argument ctx NULL");
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)ctx;
        /* Codes_SRS_IOTHUBCLIENT_LL_07_014: [ If deviceTwinCallback is NULL then IoTHubClientCore_LL_RetrievePropertyComplete shall do nothing.] */
        if (handleData->deviceTwinCallback)
        {
            /* Codes_SRS_IOTHUBCLIENT_LL_07_015: [ If the the update_state parameter is DEVICE_TWIN_UPDATE_PARTIAL and a DEVICE_TWIN_UPDATE_COMPLETE has not been previously recieved then IoTHubClientCore_LL_RetrievePropertyComplete shall do nothing.] */
            if (update_state == DEVICE_TWIN_UPDATE_COMPLETE)
            {
                handleData->complete_twin_update_encountered = true;
            }
            if (handleData->complete_twin_update_encountered)
            {
                /* Codes_SRS_IOTHUBCLIENT_LL_07_016: [ If deviceTwinCallback is set and DEVICE_TWIN_UPDATE_COMPLETE has been encountered then IoTHubClientCore_LL_RetrievePropertyComplete shall call deviceTwinCallback.] */
                handleData->deviceTwinCallback(update_state, payLoad, size, handleData->deviceTwinContextCallback);
            }
        }
    }
}

static void IoTHubClientCore_LL_ReportedStateComplete(uint32_t item_id, int status_code, void* ctx)
{
    /* Codes_SRS_IOTHUBCLIENT_LL_07_002: [ if handle or queue_handle are NULL then IoTHubClientCore_LL_ReportedStateComplete shall do nothing. ] */
    if (ctx == NULL)
    {
        /*"shall return"*/
        LogError("Invalid argument handle=%p", ctx);
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)ctx;

        /* Codes_SRS_IOTHUBCLIENT_LL_07_003: [ IoTHubClientCore_LL_ReportedStateComplete shall enumerate through the IOTHUB_DEVICE_TWIN structures in queue_handle. ]*/
        DLIST_ENTRY* client_item = handleData->iot_ack_queue.Flink;
        while (client_item != &(handleData->iot_ack_queue)) /*while we are not at the end of the list*/
        {
            PDLIST_ENTRY next_item = client_item->Flink;
            IOTHUB_DEVICE_TWIN* queue_data = containingRecord(client_item, IOTHUB_DEVICE_TWIN, entry);
            if (queue_data->item_id == item_id)
            {
                if (queue_data->reported_state_callback != NULL)
                {
                    queue_data->reported_state_callback(status_code, queue_data->context);
                }
                /*Codes_SRS_IOTHUBCLIENT_LL_07_009: [ IoTHubClientCore_LL_ReportedStateComplete shall remove the IOTHUB_DEVICE_TWIN item from the ack queue.]*/
                DList_RemoveEntryList(client_item);
                device_twin_data_destroy(queue_data);
                break;
            }
            client_item = next_item;
        }
    }
}

static void IoTHubClientCore_LL_ConnectionStatusCallBack(IOTHUB_CLIENT_CONNECTION_STATUS status, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* ctx)
{
    /*Codes_SRS_IOTHUBCLIENT_LL_25_113: [If parameter connectionStatus is NULL or parameter handle is NULL then IoTHubClientCore_LL_ConnectionStatusCallBack shall return.]*/
    if (ctx == NULL)
    {
        /*"shall return"*/
        LogError("invalid arg");
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)ctx;

        /*Codes_SRS_IOTHUBCLIENT_LL_25_114: [IoTHubClientCore_LL_ConnectionStatusCallBack shall call non-callback set by the user from IoTHubClientCore_LL_SetConnectionStatusCallback passing the status, reason and the passed userContextCallback.]*/
        if (handleData->conStatusCallback != NULL)
        {
            handleData->conStatusCallback(status, reason, handleData->conStatusUserContextCallback);
        }
    }
}

static const char* IoTHubClientCore_LL_GetProductInfo(void* ctx)
{
    const char* result;
    if (ctx == NULL)
    {
        result = NULL;
        LogError("invalid argument ctx %p", ctx);
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* iothub_data = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)ctx;
        result = STRING_c_str(iothub_data->product_info);
    }
    return result;
}

static bool IoTHubClientCore_LL_MessageCallbackFromInput(MESSAGE_CALLBACK_INFO* messageData, void* ctx)
{
    bool result;
    IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)ctx;

    if ((handleData == NULL) || messageData == NULL)
    {
        // Codes_SRS_IOTHUBCLIENT_LL_31_137: [ If either parameter `handle` or `messageData` is `NULL` then `IoTHubClient_LL_MessageCallbackFromInput` shall return `false`.** ]
        LogError("invalid argument: handle(%p), messageData(%p)", handleData, messageData);
        result = false;
    }
    else if (messageData->messageHandle == NULL)
    {
        // Codes_SRS_IOTHUBCLIENT_LL_31_137: [ If either parameter `handle` or `messageData` is `NULL` then `IoTHubClient_LL_MessageCallbackFromInput` shall return `false`.** ]
        LogError("invalid argument messageData->messageHandle(NULL)");
        result = false;
    }
    else if (handleData->event_callbacks == NULL)
    {
        LogError("Callback from input called but no input specific callbacks registered");
        result = false;
    }
    else
    {
        const char* inputName = IoTHubMessage_GetInputName(messageData->messageHandle);

        LIST_ITEM_HANDLE item_handle = NULL;

        item_handle = singlylinkedlist_find(handleData->event_callbacks, is_event_equal_for_match, (const void*)inputName);

        if (item_handle == NULL)
        {
            // Codes_SRS_IOTHUBCLIENT_LL_31_138: [ If there is no registered handler for the inputName from `IoTHubMessage_GetInputName`, then `IoTHubClient_LL_MessageCallbackFromInput` shall attempt invoke the default handler handler.** ]
            item_handle = singlylinkedlist_find(handleData->event_callbacks, is_event_equal_for_match, NULL);
        }

        if (item_handle == NULL)
        {
            LogError("Could not find callback (explicit or default) for input queue %s", inputName);
            result = false;
        }
        else
        {
            IOTHUB_EVENT_CALLBACK* event_callback = (IOTHUB_EVENT_CALLBACK*)singlylinkedlist_item_get_value(item_handle);
            if (NULL == event_callback)
            {
                LogError("singlylinkedlist_item_get_value for event_callback failed");
                result = false;
            }
            else
            {
                // Codes_SRS_IOTHUBCLIENT_LL_09_004: [IoTHubClient_LL_GetLastMessageReceiveTime shall return lastMessageReceiveTime in localtime]
                handleData->lastMessageReceiveTime = get_time(NULL);

                if (event_callback->callbackAsyncEx != NULL)
                {
                    // Codes_SRS_IOTHUBCLIENT_LL_31_139: [ `IoTHubClient_LL_MessageCallbackFromInput` shall the callback from the given inputName queue if it has been registered.** ]
                    result = event_callback->callbackAsyncEx(messageData, event_callback->userContextCallbackEx);
                }
                else
                {
                    // Codes_SRS_IOTHUBCLIENT_LL_31_139: [ `IoTHubClient_LL_MessageCallbackFromInput` shall the callback from the given inputName queue if it has been registered.** ]
                    IOTHUBMESSAGE_DISPOSITION_RESULT cb_result = event_callback->callbackAsync(messageData->messageHandle, event_callback->userContextCallback);

                    // Codes_SRS_IOTHUBCLIENT_LL_31_140: [ `IoTHubClient_LL_MessageCallbackFromInput` shall send the message disposition as returned by the client to the underlying layer and return `true` if an input queue match is found.** ]
                    if (handleData->IoTHubTransport_SendMessageDisposition(messageData, cb_result) != IOTHUB_CLIENT_OK)
                    {
                        LogError("IoTHubTransport_SendMessageDisposition failed");
                    }
                    result = true;
                }
            }
        }
    }
    return result;
}

static bool IoTHubClientCore_LL_MessageCallback(MESSAGE_CALLBACK_INFO* messageData, void* ctx)
{
    bool result;
    if ((ctx == NULL) || messageData == NULL)
    {
        /*Codes_SRS_IOTHUBCLIENT_LL_02_029: [If parameter handle is NULL then IoTHubClient_LL_MessageCallback shall return IOTHUBMESSAGE_ABANDONED.] */
        LogError("invalid argument: ctx(%p), messageData(%p)", ctx, messageData);
        result = false;
    }
    else if (messageData->messageHandle == NULL)
    {
        /*Codes_SRS_IOTHUBCLIENT_LL_10_004: [If messageHandle field of paramger messageData is NULL then IoTHubClient_LL_MessageCallback shall return IOTHUBMESSAGE_ABANDONED.] */
        LogError("invalid argument messageData->messageHandle(NULL)");
        result = false;
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)ctx;
        return invoke_message_callback(handleData, messageData);
    }
    return result;
}

static int IoTHubClientCore_LL_DeviceMethodComplete(const char* method_name, const unsigned char* payLoad, size_t size, METHOD_HANDLE response_id, void* ctx)
{
    int result;
    if (ctx == NULL)
    {
        /* Codes_SRS_IOTHUBCLIENT_LL_07_017: [ If handle or response is NULL then IoTHubClientCore_LL_DeviceMethodComplete shall return 500. ] */
        LogError("Invalid argument ctx=%p", ctx);
        result = MU_FAILURE;
    }
    else
    {
        /* Codes_SRS_IOTHUBCLIENT_LL_07_018: [ If deviceMethodCallback is not NULL IoTHubClientCore_LL_DeviceMethodComplete shall execute deviceMethodCallback and return the status. ] */
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)ctx;
        switch (handleData->methodCallback.type)
        {
            case CALLBACK_TYPE_SYNC:
            {
                unsigned char* payload_resp = NULL;
                size_t response_size = 0;
                result = handleData->methodCallback.callbackSync(method_name, payLoad, size, &payload_resp, &response_size, handleData->methodCallback.userContextCallback);
                /* Codes_SRS_IOTHUBCLIENT_LL_07_020: [ deviceMethodCallback shall build the BUFFER_HANDLE with the response payload from the IOTHUB_CLIENT_DEVICE_METHOD_CALLBACK_ASYNC callback. ] */
                if (payload_resp != NULL && response_size > 0)
                {
                    result = handleData->IoTHubTransport_DeviceMethod_Response(handleData->deviceHandle, response_id, payload_resp, response_size, result);
                }
                else
                {
                    result = MU_FAILURE;
                }
                if (payload_resp != NULL)
                {
                    free(payload_resp);
                }
                break;
            }
            case CALLBACK_TYPE_ASYNC:
                result = handleData->methodCallback.callbackAsync(method_name, payLoad, size, response_id, handleData->methodCallback.userContextCallback);
                break;
            default:
                /* Codes_SRS_IOTHUBCLIENT_LL_07_019: [ If deviceMethodCallback is NULL IoTHubClientCore_LL_DeviceMethodComplete shall return 404. ] */
                result = 0;
                break;
        }
    }
    return result;
}

static IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* construct_iothub_client(void)
{
    IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* result;

    if ((result = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)malloc(sizeof(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA))) == NULL)
    {
        LogError("failure allocating IOTHUB_CLIENT_CORE_LL_HANDLE_DATA");
    }
    else
    {
        memset(result, 0, sizeof(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA));
        if ((result->tickCounter = tickcounter_create()) == NULL)
        {
            LogError("Failure creating tickcounter object");
            free(result);
            result = NULL;
        }
        else
        {
            result->registration_state = OP_STATE_IDLE;
            /*Codes_SRS_IOTHUBCLIENT_LL_02_004: [Otherwise IoTHubClientCore_LL_Create shall initialize a new DLIST (further called "waitingToSend") containing records with fields of the following types: IOTHUB_MESSAGE_HANDLE, IOTHUB_CLIENT_EVENT_CONFIRMATION_CALLBACK, void*.]*/
            DList_InitializeListHead(&(result->waitingToSend));
            DList_InitializeListHead(&(result->iot_msg_queue));
            DList_InitializeListHead(&(result->iot_ack_queue));
            result->messageCallback.type = CALLBACK_TYPE_NONE;
            result->lastMessageReceiveTime = INDEFINITE_TIME;
            result->data_msg_id = 1;
            /*Codes_SRS_IOTHUBCLIENT_LL_02_042: [ By default, messages shall not timeout. ]*/
            result->currentMessageTimeout = 0;
            result->current_device_twin_timeout = 0;

            result->diagnostic_setting.currentMessageNumber = 0;
            result->diagnostic_setting.diagSamplingPercentage = 0;
        }
    }
    return result;
}

static void destruct_iothub_client(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* iothub_client_data)
{
    STRING_delete(iothub_client_data->product_info);
    tickcounter_destroy(iothub_client_data->tickCounter);
    free(iothub_client_data);
}

static int initialize_iothub_handle(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* iothub_handle, const IOTHUB_CONFIG_NORMALIZED* config_info, const char* module_id)
{
    int result = 0;
    // Need to initialize the auth module
    const char* device_key = NULL;
    const char* device_id = NULL;
    const char* sas_token = NULL;
    IOTHUB_CLIENT_CONFIG actual_config;
    const IOTHUB_CLIENT_CONFIG* config = NULL;
    char* IoTHubName = NULL;
    char* IoTHubSuffix = NULL;

    if (config_info->config_type == CONFIG_TYPE_DEVICE)
    {
        device_key = config_info->IOTHUB_CONFIG_ITEMS.device_config->deviceKey;
        device_id = config_info->IOTHUB_CONFIG_ITEMS.device_config->deviceId;
        sas_token = config_info->IOTHUB_CONFIG_ITEMS.device_config->deviceSasToken;
    }
    else if (config_info->config_type == CONFIG_TYPE_CLIENT)
    {
        device_key = config_info->IOTHUB_CONFIG_ITEMS.client_config->deviceKey;
        device_id = config_info->IOTHUB_CONFIG_ITEMS.client_config->deviceId;
        sas_token = config_info->IOTHUB_CONFIG_ITEMS.client_config->deviceSasToken;
    }

    if (iothub_handle->authorization_module == NULL && (iothub_handle->authorization_module = IoTHubClient_Auth_Create(device_key, device_id, sas_token, module_id)) == NULL)
    {
        LogError("Failed create authorization module");
        result = MU_FAILURE;
    }
    else
    {
        // Create the transport
        TRANSPORT_CALLBACKS_INFO transport_cb;
        memset(&transport_cb, 0, sizeof(TRANSPORT_CALLBACKS_INFO));
        transport_cb.send_complete_cb = IoTHubClientCore_LL_SendComplete;
        transport_cb.twin_retrieve_prop_complete_cb = IoTHubClientCore_LL_RetrievePropertyComplete;
        transport_cb.twin_rpt_state_complete_cb = IoTHubClientCore_LL_ReportedStateComplete;
        transport_cb.connection_status_cb = IoTHubClientCore_LL_ConnectionStatusCallBack;
        transport_cb.prod_info_cb = IoTHubClientCore_LL_GetProductInfo;
        transport_cb.msg_input_cb = IoTHubClientCore_LL_MessageCallbackFromInput;
        transport_cb.msg_cb = IoTHubClientCore_LL_MessageCallback;
        transport_cb.method_complete_cb = IoTHubClientCore_LL_DeviceMethodComplete;

        if (config_info->config_type == CONFIG_TYPE_DEVICE)
        {
            STRING_HANDLE transport_hostname = NULL;

            iothub_handle->transportHandle = config_info->IOTHUB_CONFIG_ITEMS.device_config->transportHandle;
            setTransportProtocol(iothub_handle, (TRANSPORT_PROVIDER*)config_info->IOTHUB_CONFIG_ITEMS.device_config->protocol());

            if (iothub_handle->IoTHubTransport_SetCallbackContext(iothub_handle->transportHandle, iothub_handle) != 0)
            {
                LogError("unable to set transport callbacks");
                IoTHubClient_Auth_Destroy(iothub_handle->authorization_module);
                iothub_handle->authorization_module = NULL;
                result = MU_FAILURE;
            }
            else if ((transport_hostname = iothub_handle->IoTHubTransport_GetHostname(iothub_handle->transportHandle)) == NULL)
            {
                // Codes_SRS_IOTHUBCLIENT_LL_02_097: [ If creating the data structures fails or instantiating the IOTHUB_CLIENT_LL_UPLOADTOBLOB_HANDLE fails then IoTHubClientCore_LL_CreateWithTransport shall fail and return NULL. ]
                LogError("unable to determine the transport IoTHub name");
                IoTHubClient_Auth_Destroy(iothub_handle->authorization_module);
                iothub_handle->authorization_module = NULL;
                result = MU_FAILURE;
            }
            else
            {
                const char* hostname = STRING_c_str(transport_hostname);
                // Codes_SRS_IOTHUBCLIENT_LL_02_096: [ IoTHubClientCore_LL_CreateWithTransport shall create the data structures needed to instantiate a IOTHUB_CLIENT_LL_UPLOADTOBLOB_HANDLE. ]
                /*the first '.' says where the iothubname finishes*/
                const char* whereIsDot = strchr(hostname, '.');
                if (whereIsDot == NULL)
                {
                    /*Codes_SRS_IOTHUBCLIENT_LL_02_097: [ If creating the data structures fails or instantiating the IOTHUB_CLIENT_LL_UPLOADTOBLOB_HANDLE fails then IoTHubClientCore_LL_CreateWithTransport shall fail and return NULL. ]*/
                    LogError("unable to determine the IoTHub name");
                    IoTHubClient_Auth_Destroy(iothub_handle->authorization_module);
                    iothub_handle->authorization_module = NULL;
                    result = MU_FAILURE;
                }
                else
                {
                    size_t suffix_len = strlen(whereIsDot);
                    /*Codes_SRS_IOTHUBCLIENT_LL_02_096: [ IoTHubClientCore_LL_CreateWithTransport shall create the data structures needed to instantiate a IOTHUB_CLIENT_LL_UPLOADTOBLOB_HANDLE. ]*/
                    IoTHubName = (char*)malloc(whereIsDot - hostname + 1);
                    if (IoTHubName == NULL)
                    {
                        /*Codes_SRS_IOTHUBCLIENT_LL_02_097: [ If creating the data structures fails or instantiating the IOTHUB_CLIENT_LL_UPLOADTOBLOB_HANDLE fails then IoTHubClientCore_LL_CreateWithTransport shall fail and return NULL. ]*/
                        LogError("unable to malloc");
                        IoTHubClient_Auth_Destroy(iothub_handle->authorization_module);
                        iothub_handle->authorization_module = NULL;
                        result = MU_FAILURE;
                    }
                    else if ((IoTHubSuffix = (char*)malloc(suffix_len + 1)) == NULL)
                    {
                        /*Codes_SRS_IOTHUBCLIENT_LL_02_097: [ If creating the data structures fails or instantiating the IOTHUB_CLIENT_LL_UPLOADTOBLOB_HANDLE fails then IoTHubClientCore_LL_CreateWithTransport shall fail and return NULL. ]*/
                        LogError("unable to malloc");
                        IoTHubClient_Auth_Destroy(iothub_handle->authorization_module);
                        iothub_handle->authorization_module = NULL;
                        result = MU_FAILURE;
                    }
                    else
                    {
                        memset(IoTHubName, 0, whereIsDot - hostname + 1);
                        (void)memcpy(IoTHubName, hostname, whereIsDot - hostname);
                        (void)strcpy(IoTHubSuffix, whereIsDot + 1);

                        actual_config.deviceId = config_info->IOTHUB_CONFIG_ITEMS.device_config->deviceId;
                        actual_config.deviceKey = config_info->IOTHUB_CONFIG_ITEMS.device_config->deviceKey;
                        actual_config.deviceSasToken = config_info->IOTHUB_CONFIG_ITEMS.device_config->deviceSasToken;
                        actual_config.iotHubName = IoTHubName;
                        actual_config.iotHubSuffix = IoTHubSuffix;
                        actual_config.protocol = NULL; /*irrelevant to IoTHubClientCore_LL_UploadToBlob*/
                        actual_config.protocolGatewayHostName = NULL; /*irrelevant to IoTHubClientCore_LL_UploadToBlob*/

                        config = &actual_config;

                        /*Codes_SRS_IOTHUBCLIENT_LL_02_008: [Otherwise, IoTHubClientCore_LL_Create shall succeed and return a non-NULL handle.] */
                        iothub_handle->isSharedTransport = true;
                    }
                }
            }
            STRING_delete(transport_hostname);
        }
        else if (config_info->config_type == CONFIG_TYPE_CLIENT)
        {
            IOTHUBTRANSPORT_CONFIG lowerLayerConfig;
            memset(&lowerLayerConfig, 0, sizeof(IOTHUBTRANSPORT_CONFIG));
            /*Codes_SRS_IOTHUBCLIENT_LL_02_006: [IoTHubClientCore_LL_Create shall populate a structure of type IOTHUBTRANSPORT_CONFIG with the information from config parameter and the previous DLIST and shall pass that to the underlying layer _Create function.]*/
            lowerLayerConfig.upperConfig = config_info->IOTHUB_CONFIG_ITEMS.client_config;
            lowerLayerConfig.waitingToSend = &(iothub_handle->waitingToSend);
            lowerLayerConfig.auth_module_handle = iothub_handle->authorization_module;
            lowerLayerConfig.moduleId = module_id;

            setTransportProtocol(iothub_handle, (TRANSPORT_PROVIDER*)config_info->IOTHUB_CONFIG_ITEMS.client_config->protocol());
            if ((iothub_handle->transportHandle = iothub_handle->IoTHubTransport_Create(&lowerLayerConfig, &transport_cb, iothub_handle)) == NULL)
            {
                /*Codes_SRS_IOTHUBCLIENT_LL_02_007: [If the underlaying layer _Create function fails them IoTHubClientCore_LL_Create shall fail and return NULL.] */
                LogError("underlying transport failed");
                destroy_module_method_module(iothub_handle);
                IoTHubClient_Auth_Destroy(iothub_handle->authorization_module);
                iothub_handle->authorization_module = NULL;
                result = MU_FAILURE;
            }
            // Codes_SRS_IOTHUBCLIENT_LL_25_124: [ `IoTHubClientCore_LL_Create` shall set the default retry policy as Exponential backoff with jitter and if succeed and return a `non-NULL` handle. ]*/
            else if (IoTHubClientCore_LL_SetRetryPolicy(iothub_handle, IOTHUB_CLIENT_RETRY_EXPONENTIAL_BACKOFF_WITH_JITTER, 0) != IOTHUB_CLIENT_OK)
            {
                LogError("Setting default retry policy in transport failed");
                destroy_module_method_module(iothub_handle);
                IoTHubClient_Auth_Destroy(iothub_handle->authorization_module);
                iothub_handle->authorization_module = NULL;
                result = MU_FAILURE;
            }
            else
            {
                // Codes_SRS_IOTHUBCLIENT_LL_02_008: [Otherwise, IoTHubClientCore_LL_Create shall succeed and return a non-NULL handle.] */
                iothub_handle->isSharedTransport = false;
                config = config_info->IOTHUB_CONFIG_ITEMS.client_config;
            }
        }
    }

    if (result == 0)
    {
        PLATFORM_INFO_OPTION supportedPlatformInfo;

        if ((module_id != NULL) && create_edge_handle(iothub_handle, config, module_id) != 0)
        {
            LogError("unable to create module method handle");
            if (!iothub_handle->isSharedTransport)
            {
                iothub_handle->IoTHubTransport_Destroy(iothub_handle->transportHandle);
            }
            IoTHubClient_Auth_Destroy(iothub_handle->authorization_module);
            iothub_handle->authorization_module = NULL;
            result = MU_FAILURE;
        }
        // Add extended info to product info if required
        else if (iothub_handle->IoTHubTransport_GetSupportedPlatformInfo(iothub_handle->transportHandle, &supportedPlatformInfo) != 0)
        {
            LogError("failed to get supported platform info");
            if (!iothub_handle->isSharedTransport)
            {
                iothub_handle->IoTHubTransport_Destroy(iothub_handle->transportHandle);
            }
            IoTHubClient_Auth_Destroy(iothub_handle->authorization_module);
            iothub_handle->authorization_module = NULL;
            destroy_module_method_module(iothub_handle);
            result = MU_FAILURE;
        }
        else if (iothub_handle->product_info == NULL && (iothub_handle->product_info = make_product_info(NULL, supportedPlatformInfo)) == NULL)
        {
            LogError("failed to initialize product info");
            if (!iothub_handle->isSharedTransport)
            {
                iothub_handle->IoTHubTransport_Destroy(iothub_handle->transportHandle);
            }
            IoTHubClient_Auth_Destroy(iothub_handle->authorization_module);
            iothub_handle->authorization_module = NULL;
            destroy_module_method_module(iothub_handle);
            result = MU_FAILURE;
        }
        else
        {
            IOTHUB_DEVICE_CONFIG deviceConfig;
            deviceConfig.deviceId = config->deviceId;
            deviceConfig.deviceKey = config->deviceKey;
            deviceConfig.deviceSasToken = config->deviceSasToken;
            deviceConfig.authorization_module = iothub_handle->authorization_module;
            deviceConfig.moduleId = module_id;

            /*Codes_SRS_IOTHUBCLIENT_LL_17_008: [IoTHubClientCore_LL_Create shall call the transport _Register function with a populated structure of type IOTHUB_DEVICE_CONFIG and waitingToSend list.] */
            if ((iothub_handle->deviceHandle = iothub_handle->IoTHubTransport_Register(iothub_handle->transportHandle, &deviceConfig, &(iothub_handle->waitingToSend))) == NULL)
            {
                LogError("Registering device in transport failed");
                IoTHubClient_Auth_Destroy(iothub_handle->authorization_module);
                iothub_handle->authorization_module = NULL;
                result = MU_FAILURE;

                // Codes_SRS_IOTHUBCLIENT_LL_09_010: [ If any failure occurs `IoTHubClientCore_LL_Create` shall destroy the `transportHandle` only if it has created it ]
                if (!iothub_handle->isSharedTransport)
                {
                    iothub_handle->IoTHubTransport_Destroy(iothub_handle->transportHandle);
                }
                destroy_module_method_module(iothub_handle);
            }
        }
    }
    if (IoTHubName)
    {
        free(IoTHubName);
    }
    if (IoTHubSuffix)
    {
        free(IoTHubSuffix);
    }
    return result;
}

static IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* initialize_iothub_client(const IOTHUB_CLIENT_CONFIG* client_config, const IOTHUB_CLIENT_DEVICE_CONFIG* device_config, bool use_dev_auth, const char* module_id)
{
    IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* result = NULL;

    if ((result = construct_iothub_client()) == NULL)
    {
        LogError("Failure constructing iothub client handle");
    }
    else
    {
        IOTHUB_CONFIG_NORMALIZED config_normalized;
        if (client_config != NULL)
        {
            config_normalized.config_type = CONFIG_TYPE_CLIENT;
            config_normalized.IOTHUB_CONFIG_ITEMS.client_config = client_config;
        }
        else
        {
            config_normalized.config_type = CONFIG_TYPE_DEVICE;
            config_normalized.IOTHUB_CONFIG_ITEMS.device_config = device_config;
        }

        if (use_dev_auth && (result->authorization_module = IoTHubClient_Auth_CreateFromDeviceAuth(config_normalized.IOTHUB_CONFIG_ITEMS.client_config->deviceId, module_id)) == NULL)
        {
            LogError("Failure initializing iothub handle");
            destruct_iothub_client(result);
            result = NULL;
        }
        else if (initialize_iothub_handle(result, &config_normalized, module_id) != 0)
        {
            LogError("Failure initializing iothub handle");
            destruct_iothub_client(result);
            result = NULL;
        }
        else
        {
            if (!use_dev_auth)
            {
                result->registration_state = OP_STATE_IOT_STAGE;
            }
        }
    }
    return result;
}

static uint32_t get_next_item_id(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData)
{
    if (handleData->data_msg_id+1 >= UINT32_MAX)
    {
        handleData->data_msg_id = 1;
    }
    else
    {
        handleData->data_msg_id++;
    }
    return handleData->data_msg_id;
}

static IOTHUB_DEVICE_TWIN* dev_twin_data_create(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData, uint32_t id, const unsigned char* reportedState, size_t size, IOTHUB_CLIENT_REPORTED_STATE_CALLBACK reportedStateCallback, void* userContextCallback)
{
    IOTHUB_DEVICE_TWIN* result = (IOTHUB_DEVICE_TWIN*)malloc(sizeof(IOTHUB_DEVICE_TWIN) );
    if (result != NULL)
    {
        result->report_data_handle = CONSTBUFFER_Create(reportedState, size);
        if (result->report_data_handle == NULL)
        {
            LogError("Failure allocating reported state data");
            free(result);
            result = NULL;
        }
        else
        {
            result->item_id = id;
            result->ms_timesOutAfter = 0;
            result->context = userContextCallback;
            result->reported_state_callback = reportedStateCallback;
            result->client_handle = handleData;
            result->device_handle = handleData->deviceHandle;
        }
    }
    else
    {
        LogError("Failure allocating device twin information");
    }
    return result;
}

static void on_get_device_twin_completed(DEVICE_TWIN_UPDATE_STATE update_state, const unsigned char* payLoad, size_t size, void* userContextCallback)
{
    if (userContextCallback == NULL)
    {
        LogError("Invalid argument (userContextCallback=NULL)");
    }
    else
    {
        GET_TWIN_CONTEXT* getTwinCtx = (GET_TWIN_CONTEXT*)userContextCallback;

        getTwinCtx->callback(update_state, payLoad, size, getTwinCtx->context);

        free(getTwinCtx);
    }
}

static void delete_event(IOTHUB_EVENT_CALLBACK* event_callback)
{
    STRING_delete(event_callback->inputName);
    free(event_callback->userContextCallbackEx);
    free(event_callback);
}

static void delete_event_callback(const void* item, const void* action_context, bool* continue_processing)
{
    (void)action_context;
    delete_event((IOTHUB_EVENT_CALLBACK*)item);
    *continue_processing = true;
}

static void delete_event_callback_list(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData)
{
    if (handleData->event_callbacks != NULL)
    {
        singlylinkedlist_foreach(handleData->event_callbacks, delete_event_callback, NULL);
        singlylinkedlist_destroy(handleData->event_callbacks);
        handleData->event_callbacks = NULL;
    }
}

#ifdef USE_PROV_MODULE
static void close_provisioning_info(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* iothub_handle)
{
    Prov_Device_LL_Destroy(iothub_handle->prov_handle);
    iothub_handle->prov_handle = NULL;
}

static int initialize_hsm_info(const PROVISIONING_AUTH_INFO* prov_info)
{
    int result;
    if (prov_dev_security_init(prov_info->hsm_type) != 0)
    {
        LogError("Failure initializing security hsm");
        result = MU_FAILURE;
    }
    else
    {
        if (prov_info->hsm_type == SECURE_DEVICE_TYPE_SYMMETRIC_KEY)
        {
            if (prov_info->registration_id == NULL || prov_info->symmetric_key == NULL)
            {
                LogError("Input parameter Symmetric key requires registrations id and symmetric key parameters");
                result = MU_FAILURE;
            }
            else if (prov_dev_security_init(prov_info->hsm_type) != 0)
            {
                LogError("Failure initializing security hsm");
                result = MU_FAILURE;
            }
            else
            {
                result = 0;
            }
        }
        else
        {
            result = 0;
        }
    }
    return result;
}

static int process_device_auth(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* iothub_handle, const char* iothub_uri, IOTHUB_CLIENT_CONFIG* config)
{
    int result;
    const char* iterator;
    const char* initial;
    char* iothub_name = NULL;
    char* iothub_suffix = NULL;

    // Find the iothub suffix
    initial = iterator = iothub_uri;
    while (iterator != NULL && *iterator != '\0')
    {
        if (*iterator == '.')
        {
            size_t length = iterator - initial;
            iothub_name = (char*)malloc(length + 1);
            if (iothub_name != NULL)
            {
                memset(iothub_name, 0, length + 1);
                memcpy(iothub_name, initial, length);
                config->iotHubName = iothub_name;

                length = strlen(initial) - length - 1;
                iothub_suffix = (char*)malloc(length + 1);
                if (iothub_suffix != NULL)
                {
                    memset(iothub_suffix, 0, length + 1);
                    memcpy(iothub_suffix, iterator + 1, length);
                    config->iotHubSuffix = iothub_suffix;
                    break;
                }
                else
                {
                    LogError("Failed to allocate iothub suffix");
                    free(iothub_name);
                    iothub_name = NULL;
                    result = MU_FAILURE;
                }
            }
            else
            {
                LogError("Failed to allocate iothub name");
                result = MU_FAILURE;
            }
        }
        iterator++;
    }

    if (config->iotHubName == NULL || config->iotHubSuffix == NULL)
    {
        LogError("initialize iothub client name");
        iothub_handle->registration_state = OP_STATE_ERROR;
        result = MU_FAILURE;
    }
    else
    {
        IOTHUB_CONFIG_NORMALIZED config_normalized;
        config_normalized.config_type = CONFIG_TYPE_CLIENT;
        config_normalized.IOTHUB_CONFIG_ITEMS.client_config = config;

        if (initialize_iothub_handle(iothub_handle, &config_normalized, NULL) != 0)
        {
            LogError("initialize iothub client");
            iothub_handle->registration_state = OP_STATE_ERROR;
            result = MU_FAILURE;
        }
        else
        {
            result = 0;
        }
    }

    free(iothub_name);
    free(iothub_suffix);
    return result;
}

static void provisioning_status_callback(PROV_DEVICE_REG_STATUS reg_status, void* user_context)
{
    if (user_context != NULL)
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)user_context;
        if (handleData->log_trace)
        {
            LOG(AZ_LOG_TRACE, LOG_LINE, "Device Provisioning: %s", MU_ENUM_TO_STRING(PROV_DEVICE_REG_STATUS, reg_status));
            if (reg_status == PROV_DEVICE_REG_STATUS_ASSIGNED)
            {
                // Space out before and after provisioning
                LOG(AZ_LOG_TRACE, LOG_LINE, "");
            }
        }
    }
}

static void provisioning_device_callback(PROV_DEVICE_RESULT register_result, const char* iothub_uri, const char* device_id, void* user_context)
{
    if (user_context == NULL)
    {
        LogError("Failure registering device: user_context is NULL\r\n");
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* iothub_handle = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)user_context;
        if (register_result != PROV_DEVICE_RESULT_OK)
        {
            LogError("Failed attempting to register with Provisioning service");
            iothub_handle->registration_state = OP_STATE_ERROR;
        }
        else
        {
            if ((iothub_handle->authorization_module = IoTHubClient_Auth_CreateFromDeviceAuth(device_id, NULL)) == NULL)
            {
                LogError("Failed create authorization module");
                iothub_handle->registration_state = OP_STATE_ERROR;
            }
            else
            {
                IOTHUB_CLIENT_CONFIG config;
                memset(&config, 0, sizeof(IOTHUB_CLIENT_CONFIG));
                config.protocol = iothub_handle->iothub_provider;
                config.deviceId = device_id;

                if (process_device_auth(iothub_handle, iothub_uri, &config) != 0)
                {
                    LogError("Failed processing device auth");
                    iothub_handle->registration_state = OP_STATE_ERROR;
                }
                else if (iothub_handle->IoTHubTransport_SetOption(iothub_handle->transportHandle, OPTION_LOG_TRACE, &iothub_handle->log_trace) != IOTHUB_CLIENT_OK)
                {
                    LogError("Failed processing device auth");
                    iothub_handle->registration_state = OP_STATE_ERROR;
                }
                else
                {
                    iothub_handle->registration_state = OP_STATE_REGISTERED;
                }
            }
        }
    }
}

static int initialize_queued_iothub_handle(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData)
{
    int result = IOTHUB_CLIENT_OK;
    // Initialize Iothub Items
    if (handleData->iothub_op_init & IOTHUB_OP_C2D_SUB)
    {
        if (handleData->IoTHubTransport_Subscribe(handleData->deviceHandle) != 0)
        {
            LogError("IoTHubTransport_Subscribe failed");
            handleData->messageCallback.type = CALLBACK_TYPE_NONE;
            handleData->messageCallback.callbackSync = NULL;
            handleData->messageCallback.callbackAsync = NULL;
            handleData->messageCallback.userContextCallback = NULL;
        }
    }
    
    if (handleData->iothub_op_init & IOTHUB_OP_MSG_DISPOSITION)
    {
        //if (handleData->IoTHubTransport_SendMessageDisposition(message_data, disposition) !=

    }
    
    if (handleData->iothub_op_init & IOTHUB_OP_DEVICE_TWIN_SUB)
    {
        if (handleData->IoTHubTransport_Subscribe_DeviceTwin(handleData->transportHandle) != 0)
        {
            LogError("IoTHubTransport_Subscribe_DeviceTwin failed");
            result = IOTHUB_CLIENT_ERROR;
        }
    }

    if (handleData->iothub_op_init & IOTHUB_OP_METHOD_SUB)
    {
        if (handleData->IoTHubTransport_Subscribe_DeviceMethod(handleData->deviceHandle) != 0)
        {
            LogError("IoTHubTransport_Subscribe_DeviceMethod failed");
            handleData->methodCallback.type = CALLBACK_TYPE_NONE;
            handleData->methodCallback.callbackAsync = NULL;
            handleData->methodCallback.callbackSync = NULL;
            handleData->methodCallback.userContextCallback = NULL;
            result = IOTHUB_CLIENT_ERROR;
        }
    }
    return result;
}

static void process_provisioning_dowork(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData)
{
    Prov_Device_LL_DoWork(handleData->prov_handle);
    if (handleData->registration_state == OP_STATE_REGISTERED)
    {
        close_provisioning_info(handleData);
        
        initialize_queued_iothub_handle(handleData);

        handleData->registration_state = OP_STATE_IOT_STAGE;
    }
    else if (handleData->registration_state == OP_STATE_ERROR)
    {
        // Call the connection status callback?
        IoTHubClientCore_LL_ConnectionStatusCallBack(IOTHUB_CLIENT_CONNECTION_UNAUTHENTICATED, IOTHUB_CLIENT_PROVISIONING_FAILED, handleData);
    }
}
#endif

static void DoTimeouts(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData)
{
    tickcounter_ms_t nowTick;
    if (tickcounter_get_current_ms(handleData->tickCounter, &nowTick) != 0)
    {
        LogError("unable to get the current ms, timeouts will not be processed");
    }
    else
    {
        DLIST_ENTRY* currentItemInWaitingToSend = handleData->waitingToSend.Flink;
        while (currentItemInWaitingToSend != &(handleData->waitingToSend)) /*while we are not at the end of the list*/
        {
            IOTHUB_MESSAGE_LIST* fullEntry = containingRecord(currentItemInWaitingToSend, IOTHUB_MESSAGE_LIST, entry);
            /*Codes_SRS_IOTHUBCLIENT_LL_02_041: [ If more than value miliseconds have passed since the call to IoTHubClientCore_LL_SendEventAsync then the message callback shall be called with a status code of IOTHUB_CLIENT_CONFIRMATION_TIMEOUT. ]*/
            if ((fullEntry->ms_timesOutAfter != 0) && ((nowTick - fullEntry->ms_timesOutAfter) > fullEntry->message_timeout_value))
            {
                PDLIST_ENTRY theNext = currentItemInWaitingToSend->Flink; /*need to save the next item, because the below operations are destructive*/
                DList_RemoveEntryList(currentItemInWaitingToSend);
                if (fullEntry->callback != NULL)
                {
                    fullEntry->callback(IOTHUB_CLIENT_CONFIRMATION_MESSAGE_TIMEOUT, fullEntry->context);
                }
                IoTHubMessage_Destroy(fullEntry->messageHandle); /*because it has been cloned*/
                free(fullEntry);
                currentItemInWaitingToSend = theNext;
            }
            else
            {
                currentItemInWaitingToSend = currentItemInWaitingToSend->Flink;
            }
        }
    }
}

static void process_iothub_dowork(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData)
{
    DoTimeouts(handleData);

    /*Codes_SRS_IOTHUBCLIENT_LL_07_008: [ IoTHubClientCore_LL_DoWork shall iterate the message queue and execute the underlying transports IoTHubTransport_ProcessItem function for each item. ] */
    DLIST_ENTRY* client_item = handleData->iot_msg_queue.Flink;
    while (client_item != &(handleData->iot_msg_queue)) /*while we are not at the end of the list*/
    {
        PDLIST_ENTRY next_item = client_item->Flink;

        IOTHUB_DEVICE_TWIN* queue_data = containingRecord(client_item, IOTHUB_DEVICE_TWIN, entry);
        IOTHUB_IDENTITY_INFO identity_info;
        identity_info.device_twin = queue_data;
        IOTHUB_PROCESS_ITEM_RESULT process_results = handleData->IoTHubTransport_ProcessItem(handleData->transportHandle, IOTHUB_TYPE_DEVICE_TWIN, &identity_info);
        if (process_results == IOTHUB_PROCESS_CONTINUE || process_results == IOTHUB_PROCESS_NOT_CONNECTED)
        {
            /*Codes_SRS_IOTHUBCLIENT_LL_07_010: [ If 'IoTHubTransport_ProcessItem' returns IOTHUB_PROCESS_CONTINUE or IOTHUB_PROCESS_NOT_CONNECTED IoTHubClientCore_LL_DoWork shall continue on to call the underlaying layer's _DoWork function. ]*/
            break;
        }
        else
        {
            DList_RemoveEntryList(client_item);
            if (process_results == IOTHUB_PROCESS_OK)
            {
                /*Codes_SRS_IOTHUBCLIENT_LL_07_011: [ If 'IoTHubTransport_ProcessItem' returns IOTHUB_PROCESS_OK IoTHubClientCore_LL_DoWork shall add the IOTHUB_DEVICE_TWIN to the ack queue. ]*/
                DList_InsertTailList(&(handleData->iot_ack_queue), &(queue_data->entry));
            }
            else
            {
                /*Codes_SRS_IOTHUBCLIENT_LL_07_012: [ If 'IoTHubTransport_ProcessItem' returns any other value IoTHubClientCore_LL_DoWork shall destroy the IOTHUB_DEVICE_TWIN item. ]*/
                LogError("Failure queue processing item");
                device_twin_data_destroy(queue_data);
            }
        }
        // Move along to the next item
        client_item = next_item;
    }

    /*Codes_SRS_IOTHUBCLIENT_LL_02_021: [Otherwise, IoTHubClientCore_LL_DoWork shall invoke the underlaying layer's _DoWork function.]*/
    handleData->IoTHubTransport_DoWork(handleData->transportHandle);
}

IOTHUB_CLIENT_CORE_LL_HANDLE IoTHubClientCore_LL_CreateFromProvisioning(const PROVISIONING_AUTH_INFO* prov_info)
{
    IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* result;
    if (prov_info == NULL || prov_info->provisioning_uri == NULL || prov_info->id_scope == NULL || prov_info->transport == NULL)
    {
        LogError("Input parameter is NULL: prov_uri: %p  protocol: %p scope_id: %p", prov_info->provisioning_uri, prov_info->transport, prov_info->id_scope);
        result = NULL;
    }
    else
    {
#ifdef USE_PROV_MODULE
        if (initialize_hsm_info(prov_info) != 0)
        {
            LogError("Failure initializing provisioning hsm");
            result = NULL;
        }
        else if ((result = construct_iothub_client()) == NULL)
        {
            LogError("Failure constructing iothub client");
        }
        else if ((result->prov_handle = Prov_Device_LL_Create(prov_info->provisioning_uri, prov_info->id_scope, prov_info->transport()->prov_provider)) == NULL)
        {
            LogError("Failure constructing provisioning client");
            destruct_iothub_client(result);
            result = NULL;
        }
        else if (Prov_Device_LL_Register_Device(result->prov_handle, provisioning_device_callback, result, provisioning_status_callback, result) != PROV_DEVICE_RESULT_OK)
        {
            Prov_Device_LL_Destroy(result->prov_handle);
            destruct_iothub_client(result);
            result = NULL;
        }
        else
        {
            // Set the state as registering
            result->registration_state = OP_STATE_REGISTERING;
            result->iothub_provider = prov_info->transport()->iothub_provider;
        }
#else
        LogError("HSM module is not included");
        result = NULL;
#endif
    }
    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SetProvisioningPayload(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, const char* json)
{
    IOTHUB_CLIENT_RESULT result;
    if (iotHubClientHandle == NULL || json == NULL)
    {
        LogError("Input parameter is NULL: iotHubClientHandle == %p || json: %p", iotHubClientHandle, json);
        result = IOTHUB_CLIENT_INVALID_ARG;
    }
    else if (iotHubClientHandle->registration_state != OP_STATE_IDLE)
    {
        LogError("IoTHubDeviceClient_LL_SetProvisioningPayload called after registration has begun");
        result = IOTHUB_CLIENT_ERROR;
    }
    else
    {
#ifdef USE_PROV_MODULE
        if (Prov_Device_LL_Set_Provisioning_Payload(iotHubClientHandle->prov_handle, json) != PROV_DEVICE_RESULT_OK)
        {
            LogError("Failure setting provisioning payload");
            result = IOTHUB_CLIENT_ERROR;
        }
        else
        {
            result = IOTHUB_CLIENT_OK;
        }
#else
        LogError("HSM module is not included");
        result = IOTHUB_CLIENT_ERROR;
#endif
    }
    return result;
}

const char* IoTHubClientCore_LL_GetProvisioningPayload(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle)
{
    const char* result;
    if (iotHubClientHandle == NULL)
    {
        LogError("Input parameter iotHubClientHandle is NULL");
        result = NULL;
    }
    else
    {
        result = NULL;
    }
    return result;
}

IOTHUB_CLIENT_CORE_LL_HANDLE IoTHubClientCore_LL_CreateFromDeviceAuth(const char* iothub_uri, const char* device_id, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* result;
    if (iothub_uri == NULL || protocol == NULL || device_id == NULL)
    {
        LogError("Input parameter is NULL: iothub_uri: %p  protocol: %p device_id: %p", iothub_uri, protocol, device_id);
        result = NULL;
    }
    else
    {
#ifdef USE_PROV_MODULE
        if ((result = construct_iothub_client()) == NULL)
        {
            LogError("Failed create iothub client");
        }
        else if ((result->authorization_module = IoTHubClient_Auth_CreateFromDeviceAuth(device_id, NULL)) == NULL)
        {
            LogError("Failed create authorization module");
            destruct_iothub_client(result);
        }
        else
        {
            IOTHUB_CLIENT_CONFIG config;

            memset(&config, 0, sizeof(IOTHUB_CLIENT_CONFIG));
            config.protocol = protocol;
            config.deviceId = device_id;

            if (process_device_auth(result, iothub_uri, &config) != 0)
            {
                LogError("Failed processing device auth info");
                IoTHubClient_Auth_Destroy(result->authorization_module);
                destruct_iothub_client(result);
            }
        }
#else
        LogError("HSM module is not included");
        result = NULL;
#endif
    }
    return result;
}

IOTHUB_CLIENT_CORE_LL_HANDLE IoTHubClientCore_LL_CreateFromConnectionString(const char* connectionString, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    IOTHUB_CLIENT_CORE_LL_HANDLE result;

    /* Codes_SRS_IOTHUBCLIENT_LL_12_003: [IoTHubClientCore_LL_CreateFromConnectionString shall verify the input parameter and if it is NULL then return NULL] */
    if (connectionString == NULL || protocol == NULL)
    {
        LogError("Input parameter is NULL: connectionString: %p protocol: %p", connectionString, protocol);
        result = NULL;
    }
    else
    {
        /* Codes_SRS_IOTHUBCLIENT_LL_12_004: [IoTHubClientCore_LL_CreateFromConnectionString shall allocate IOTHUB_CLIENT_CONFIG structure] */
        IOTHUB_CLIENT_CONFIG config;
        STRING_TOKENIZER_HANDLE tokenizer1 = NULL;
        STRING_HANDLE tokenString = NULL;
        STRING_HANDLE valueString = NULL;
        STRING_HANDLE hostNameString = NULL;
        STRING_HANDLE hostSuffixString = NULL;
        STRING_HANDLE deviceIdString = NULL;
        STRING_HANDLE deviceKeyString = NULL;
        STRING_HANDLE deviceSasTokenString = NULL;
        STRING_HANDLE protocolGateway = NULL;
        STRING_HANDLE moduleId = NULL;

        memset(&config, 0, sizeof(config));
        config.protocol = protocol;

        /* Codes_SRS_IOTHUBCLIENT_LL_04_002: [If it does not, it shall pass the protocolGatewayHostName NULL.] */
        config.protocolGatewayHostName = NULL;

        if ((tokenizer1 = STRING_TOKENIZER_create_from_char(connectionString)) == NULL)
        {
            LogError("Error creating Tokenizer");
            result = NULL;
        }
        else if ((tokenString = STRING_new()) == NULL)
        {
            LogError("Error creating Token String");
            result = NULL;
        }
        else if ((valueString = STRING_new()) == NULL)
        {
            LogError("Error creating Value String");
            result = NULL;
        }
        else if ((hostNameString = STRING_new()) == NULL)
        {
            LogError("Error creating HostName String");
            result = NULL;
        }
        else if ((hostSuffixString = STRING_new()) == NULL)
        {
            LogError("Error creating HostSuffix String");
            result = NULL;
        }
        /* Codes_SRS_IOTHUBCLIENT_LL_12_005: [IoTHubClientCore_LL_CreateFromConnectionString shall try to parse the connectionString input parameter for the following structure: "Key1=value1;key2=value2;key3=value3..."] */
        /* Codes_SRS_IOTHUBCLIENT_LL_12_006: [IoTHubClientCore_LL_CreateFromConnectionString shall verify the existence of the following Key/Value pairs in the connection string: HostName, DeviceId, SharedAccessKey, SharedAccessSignature or x509]  */
        else
        {
            int isx509found = 0;
            bool use_provisioning = false;
            while ((STRING_TOKENIZER_get_next_token(tokenizer1, tokenString, "=") == 0))
            {
                if (STRING_TOKENIZER_get_next_token(tokenizer1, valueString, ";") != 0)
                {
                    LogError("Tokenizer error");
                    break;
                }
                else
                {
                    if (tokenString != NULL)
                    {
                        /* Codes_SRS_IOTHUBCLIENT_LL_12_010: [IoTHubClientCore_LL_CreateFromConnectionString shall fill up the IOTHUB_CLIENT_CONFIG structure using the following mapping: iotHubName = Name, iotHubSuffix = Suffix, deviceId = DeviceId, deviceKey = SharedAccessKey or deviceSasToken = SharedAccessSignature] */
                        const char* s_token = STRING_c_str(tokenString);
                        if (strcmp(s_token, HOSTNAME_TOKEN) == 0)
                        {
                            /* Codes_SRS_IOTHUBCLIENT_LL_12_009: [IoTHubClientCore_LL_CreateFromConnectionString shall split the value of HostName to Name and Suffix using the first "." as a separator] */
                            STRING_TOKENIZER_HANDLE tokenizer2 = NULL;
                            if ((tokenizer2 = STRING_TOKENIZER_create(valueString)) == NULL)
                            {
                                LogError("Error creating Tokenizer");
                                break;
                            }
                            else
                            {
                                /* Codes_SRS_IOTHUBCLIENT_LL_12_015: [If the string split failed, IoTHubClientCore_LL_CreateFromConnectionString returns NULL ] */
                                if (STRING_TOKENIZER_get_next_token(tokenizer2, hostNameString, ".") != 0)
                                {
                                    LogError("Tokenizer error");
                                    STRING_TOKENIZER_destroy(tokenizer2);
                                    break;
                                }
                                else
                                {
                                    config.iotHubName = STRING_c_str(hostNameString);
                                    if (STRING_TOKENIZER_get_next_token(tokenizer2, hostSuffixString, ";") != 0)
                                    {
                                        LogError("Tokenizer error");
                                        STRING_TOKENIZER_destroy(tokenizer2);
                                        break;
                                    }
                                    else
                                    {
                                        config.iotHubSuffix = STRING_c_str(hostSuffixString);
                                    }
                                }
                                STRING_TOKENIZER_destroy(tokenizer2);
                            }
                        }
                        else if (strcmp(s_token, DEVICEID_TOKEN) == 0)
                        {
                            deviceIdString = STRING_clone(valueString);
                            if (deviceIdString != NULL)
                            {
                                config.deviceId = STRING_c_str(deviceIdString);
                            }
                            else
                            {
                                LogError("Failure cloning device id string");
                                break;
                            }
                        }
                        else if (strcmp(s_token, DEVICEKEY_TOKEN) == 0)
                        {
                            deviceKeyString = STRING_clone(valueString);
                            if (deviceKeyString != NULL)
                            {
                                config.deviceKey = STRING_c_str(deviceKeyString);
                            }
                            else
                            {
                                LogError("Failure cloning device key string");
                                break;
                            }
                        }
                        else if (strcmp(s_token, DEVICESAS_TOKEN) == 0)
                        {
                            deviceSasTokenString = STRING_clone(valueString);
                            if (deviceSasTokenString != NULL)
                            {
                                config.deviceSasToken = STRING_c_str(deviceSasTokenString);
                            }
                            else
                            {
                                LogError("Failure cloning device sasToken string");
                                break;
                            }
                        }
                        else if (strcmp(s_token, X509_TOKEN) == 0)
                        {
                            if (strcmp(STRING_c_str(valueString), X509_TOKEN_ONLY_ACCEPTABLE_VALUE) != 0)
                            {
                                LogError("x509 option has wrong value, the only acceptable one is \"true\"");
                                break;
                            }
                            else
                            {
                                isx509found = 1;
                            }
                        }
                        else if (strcmp(s_token, PROVISIONING_TOKEN) == 0)
                        {
                            if (strcmp(STRING_c_str(valueString), PROVISIONING_ACCEPTABLE_VALUE) != 0)
                            {
                                LogError("provisioning option has wrong value, the only acceptable one is \"true\"");
                                break;
                            }
                            else
                            {
                                use_provisioning = 1;
                            }
                        }

                        /* Codes_SRS_IOTHUBCLIENT_LL_04_001: [IoTHubClientCore_LL_CreateFromConnectionString shall verify the existence of key/value pair GatewayHostName. If it does exist it shall pass the value to IoTHubClientCore_LL_Create API.] */
                        else if (strcmp(s_token, PROTOCOL_GATEWAY_HOST_TOKEN) == 0)
                        {
                            protocolGateway = STRING_clone(valueString);
                            if (protocolGateway != NULL)
                            {
                                config.protocolGatewayHostName = STRING_c_str(protocolGateway);
                            }
                            else
                            {
                                LogError("Failure cloning protocol Gateway Name");
                                break;
                            }
                        }
                        /*Codes_SRS_IOTHUBCLIENT_LL_31_126: [IoTHubClient_LL_CreateFromConnectionString shall optionally parse ModuleId, if present.] */
                        else if (strcmp(s_token, MODULE_ID_TOKEN) == 0)
                        {
                            moduleId = STRING_clone(valueString);
                            if (moduleId == NULL)
                            {
                                LogError("Failure cloning moduleId string");
                                break;
                            }
                        }
                        else
                        {
                            // If we get an unknown token, log it to error stream but do not cause a fatal error.
                            LogError("Unknown token <%s> in connection string.  Ignoring error and continuing to parse", s_token);
                        }
                    }
                }
            }
            /* parsing is done - check the result */
            if (config.iotHubName == NULL)
            {
                LogError("iotHubName is not found");
                result = NULL;
            }
            else if (config.iotHubSuffix == NULL)
            {
                LogError("iotHubSuffix is not found");
                result = NULL;
            }
            else if (config.deviceId == NULL)
            {
                LogError("deviceId is not found");
                result = NULL;
            }
            else if (!(
                ((!use_provisioning && !isx509found) && (config.deviceSasToken == NULL) ^ (config.deviceKey == NULL)) ||
                ((use_provisioning || isx509found) && (config.deviceSasToken == NULL) && (config.deviceKey == NULL))
                ))
            {
                LogError("invalid combination of x509, provisioning, deviceSasToken and deviceKey");
                result = NULL;
            }
            else
            {
                /* Codes_SRS_IOTHUBCLIENT_LL_12_011: [IoTHubClientCore_LL_CreateFromConnectionString shall call into the IoTHubClientCore_LL_Create API with the current structure and returns with the return value of it] */
                result = initialize_iothub_client(&config, NULL, use_provisioning, STRING_c_str(moduleId));
                if (result == NULL)
                {
                    LogError("IoTHubClientCore_LL_Create failed");
                }
                else
                {
                    /*return as is*/
                }
            }
        }
        if (deviceSasTokenString != NULL)
            STRING_delete(deviceSasTokenString);
        if (deviceKeyString != NULL)
            STRING_delete(deviceKeyString);
        if (deviceIdString != NULL)
            STRING_delete(deviceIdString);
        if (hostSuffixString != NULL)
            STRING_delete(hostSuffixString);
        if (hostNameString != NULL)
            STRING_delete(hostNameString);
        if (valueString != NULL)
            STRING_delete(valueString);
        if (tokenString != NULL)
            STRING_delete(tokenString);
        if (protocolGateway != NULL)
            STRING_delete(protocolGateway);
        if (moduleId != NULL)
            STRING_delete(moduleId);

        if (tokenizer1 != NULL)
            STRING_TOKENIZER_destroy(tokenizer1);
    }
    return result;
}

static IOTHUB_CLIENT_CORE_LL_HANDLE IoTHubClientCore_LL_CreateImpl(const IOTHUB_CLIENT_CONFIG* config, const char* module_id, bool use_dev_auth)
{
    IOTHUB_CLIENT_CORE_LL_HANDLE result;
    /*Codes_SRS_IOTHUBCLIENT_LL_02_001: [IoTHubClientCore_LL_Create shall return NULL if config parameter is NULL or protocol field is NULL.]*/
    if(
        (config == NULL) ||
        (config->protocol == NULL)
        )
    {
        result = NULL;
        LogError("invalid configuration (NULL detected)");
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = initialize_iothub_client(config, NULL, use_dev_auth, module_id);
        if (handleData == NULL)
        {
            LogError("initialize iothub client");
            result = NULL;
        }
        else
        {
            result = handleData;
        }
    }

    return result;
}

IOTHUB_CLIENT_CORE_LL_HANDLE IoTHubClientCore_LL_Create(const IOTHUB_CLIENT_CONFIG* config)
{
    return IoTHubClientCore_LL_CreateImpl(config, NULL, false);
}

#ifdef USE_EDGE_MODULES
IOTHUB_CLIENT_CORE_LL_HANDLE IoTHubClientCore_LL_CreateFromEnvironment(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* result;
    EDGE_ENVIRONMENT_VARIABLES edge_environment_variables;

    memset(&edge_environment_variables, 0, sizeof(edge_environment_variables));

    if (retrieve_edge_environment_variabes(&edge_environment_variables) != 0)
    {
        LogError("retrieve_edge_environment_variabes failed");
        result = NULL;
    }
    // The presence of a connection string environment variable means we use it, ignoring other settings
    else if (edge_environment_variables.connection_string != NULL)
    {
        if ((result = IoTHubClientCore_LL_CreateFromConnectionString(edge_environment_variables.connection_string, protocol)) == NULL)
        {
            LogError("IoTHubClientCore_LL_CreateFromConnectionString fails");
        }
    }
    else if (iothub_security_init(IOTHUB_SECURITY_TYPE_HTTP_EDGE) != 0)
    {
        LogError("iothub_security_init failed");
        result = NULL;
    }
    else
    {
        IOTHUB_CLIENT_CONFIG client_config;

        memset(&client_config, 0, sizeof(client_config));
        client_config.protocol = protocol;
        client_config.deviceId = edge_environment_variables.device_id;
        client_config.iotHubName = edge_environment_variables.iothub_name;
        client_config.iotHubSuffix = edge_environment_variables.iothub_suffix;
        client_config.protocolGatewayHostName = edge_environment_variables.gatewayhostname;

        if ((result = IoTHubClientCore_LL_CreateImpl(&client_config, edge_environment_variables.module_id, true)) == NULL)
        {
            LogError("IoTHubClientCore_LL_CreateImpl fails");
        }
    }

    if (result != NULL)
    {
        // Because the Edge Hub almost always use self-signed certificates, we need to specify which certificates to trust.  We need to do
        // this regardless of how we created the underlying IOTHUB_CLIENT_CORE_LL_HANDLE_DATA.
        IOTHUB_CLIENT_RESULT setTrustResult;
        char* trustedCertificate = IoTHubClient_Auth_Get_TrustBundle(result->authorization_module, edge_environment_variables.ca_trusted_certificate_file);

        if (trustedCertificate == NULL)
        {
            LogError("IoTHubClient_Auth_Get_TrustBundle failed");
            IoTHubClientCore_LL_Destroy(result);
            result = NULL;
        }
        else if ((setTrustResult = IoTHubClientCore_LL_SetOption(result, OPTION_TRUSTED_CERT, trustedCertificate)) != IOTHUB_CLIENT_OK)
        {
            LogError("IoTHubClientCore_LL_SetOption failed, err = %d", setTrustResult);
            IoTHubClientCore_LL_Destroy(result);
            result = NULL;
        }

        free(trustedCertificate);
    }

    free(edge_environment_variables.iothub_buffer);
    return result;
}
#endif

IOTHUB_CLIENT_CORE_LL_HANDLE IoTHubClientCore_LL_CreateWithTransport(const IOTHUB_CLIENT_DEVICE_CONFIG * config)
{
    IOTHUB_CLIENT_CORE_LL_HANDLE result;
    /*Codes_SRS_IOTHUBCLIENT_LL_17_001: [IoTHubClientCore_LL_CreateWithTransport shall return NULL if config parameter is NULL, or protocol field is NULL or transportHandle is NULL.]*/
    if (
        (config == NULL) ||
        (config->protocol == NULL) ||
        (config->transportHandle == NULL) ||
        /*Codes_SRS_IOTHUBCLIENT_LL_02_098: [ IoTHubClientCore_LL_CreateWithTransport shall fail and return NULL if both config->deviceKey AND config->deviceSasToken are NULL. ]*/
        ((config->deviceKey == NULL) && (config->deviceSasToken == NULL))
        )
    {
        result = NULL;
        LogError("invalid configuration (NULL detected)");
    }
    else
    {
        result = initialize_iothub_client(NULL, config, false, NULL);
    }
    return result;
}

void IoTHubClientCore_LL_Destroy(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle)
{
    /*Codes_SRS_IOTHUBCLIENT_LL_02_009: [IoTHubClientCore_LL_Destroy shall do nothing if parameter iotHubClientHandle is NULL.]*/
    if (iotHubClientHandle != NULL)
    {
        PDLIST_ENTRY unsend;
        /*Codes_SRS_IOTHUBCLIENT_LL_17_010: [IoTHubClientCore_LL_Destroy  shall call the underlaying layer's _Unregister function] */
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;

        if (handleData->IoTHubTransport_Unregister != NULL)
        {
            handleData->IoTHubTransport_Unregister(handleData->deviceHandle);
        }
        if (handleData->isSharedTransport == false && handleData->IoTHubTransport_Destroy != NULL)
        {
            /*Codes_SRS_IOTHUBCLIENT_LL_02_010: [If iotHubClientHandle was not created by IoTHubClientCore_LL_CreateWithTransport, IoTHubClientCore_LL_Destroy  shall call the underlaying layer's _Destroy function.] */
            handleData->IoTHubTransport_Destroy(handleData->transportHandle);
        }
        /*if any, remove the items currently not send*/
        while ((unsend = DList_RemoveHeadList(&(handleData->waitingToSend))) != &(handleData->waitingToSend))
        {
            IOTHUB_MESSAGE_LIST* temp = containingRecord(unsend, IOTHUB_MESSAGE_LIST, entry);
            /*Codes_SRS_IOTHUBCLIENT_LL_02_033: [Otherwise, IoTHubClientCore_LL_Destroy shall complete all the event message callbacks that are in the waitingToSend list with the result IOTHUB_CLIENT_CONFIRMATION_BECAUSE_DESTROY.] */
            if (temp->callback != NULL)
            {
                temp->callback(IOTHUB_CLIENT_CONFIRMATION_BECAUSE_DESTROY, temp->context);
            }
            IoTHubMessage_Destroy(temp->messageHandle);
            free(temp);
        }

        /* Codes_SRS_IOTHUBCLIENT_LL_07_007: [ IoTHubClientCore_LL_Destroy shall iterate the device twin queues and destroy any remaining items. ] */
        while ((unsend = DList_RemoveHeadList(&(handleData->iot_msg_queue))) != &(handleData->iot_msg_queue))
        {
            IOTHUB_DEVICE_TWIN* temp = containingRecord(unsend, IOTHUB_DEVICE_TWIN, entry);
            device_twin_data_destroy(temp);
        }
        while ((unsend = DList_RemoveHeadList(&(handleData->iot_ack_queue))) != &(handleData->iot_ack_queue))
        {
            IOTHUB_DEVICE_TWIN* temp = containingRecord(unsend, IOTHUB_DEVICE_TWIN, entry);
            device_twin_data_destroy(temp);
        }

        /* Codes_SRS_IOTHUBCLIENT_LL_31_141: [ IoTHubClient_LL_Destroy shall iterate registered callbacks for input queues and destroy any remaining items. ] */
        delete_event_callback_list(handleData);

        /*Codes_SRS_IOTHUBCLIENT_LL_17_011: [IoTHubClientCore_LL_Destroy  shall free the resources allocated by IoTHubClient (if any).] */
        IoTHubClient_Auth_Destroy(handleData->authorization_module);
        tickcounter_destroy(handleData->tickCounter);
#ifndef DONT_USE_UPLOADTOBLOB
        IoTHubClient_LL_UploadToBlob_Destroy(handleData->uploadToBlobHandle);
#endif
#ifdef USE_EDGE_MODULES
        IoTHubClient_EdgeHandle_Destroy(handleData->methodHandle);
#endif
        STRING_delete(handleData->product_info);
        free(handleData);
    }
}

/*Codes_SRS_IOTHUBCLIENT_LL_02_044: [ Messages already delivered to IoTHubClientCore_LL shall not have their timeouts modified by a new call to IoTHubClientCore_LL_SetOption. ]*/
/*returns 0 on success, any other value is error*/
static int attach_ms_timesOutAfter(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData, IOTHUB_MESSAGE_LIST *newEntry)
{
    int result;
    /*Codes_SRS_IOTHUBCLIENT_LL_02_043: [ Calling IoTHubClientCore_LL_SetOption with value set to "0" shall disable the timeout mechanism for all new messages. ]*/
    if (handleData->currentMessageTimeout == 0)
    {
        newEntry->ms_timesOutAfter = 0; /*do not timeout*/
        newEntry->message_timeout_value = 0;
        result = 0;
    }
    else
    {
        /*Codes_SRS_IOTHUBCLIENT_LL_02_039: [ "messageTimeout" - once IoTHubClientCore_LL_SendEventAsync is called the message shall timeout after value miliseconds. Value is a pointer to a tickcounter_ms_t. ]*/
        if (tickcounter_get_current_ms(handleData->tickCounter, &newEntry->ms_timesOutAfter) != 0)
        {
            result = MU_FAILURE;
            LogError("unable to get the current relative tickcount");
        }
        else
        {
            newEntry->message_timeout_value = handleData->currentMessageTimeout;
            result = 0;
        }
    }
    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SendEventAsync(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, IOTHUB_MESSAGE_HANDLE eventMessageHandle, IOTHUB_CLIENT_EVENT_CONFIRMATION_CALLBACK eventConfirmationCallback, void* userContextCallback)
{
    IOTHUB_CLIENT_RESULT result;
    /*Codes_SRS_IOTHUBCLIENT_LL_02_011: [IoTHubClientCore_LL_SendEventAsync shall fail and return IOTHUB_CLIENT_INVALID_ARG if parameter iotHubClientHandle or eventMessageHandle is NULL.]*/
    if (
        (iotHubClientHandle == NULL) ||
        (eventMessageHandle == NULL) ||
        /*Codes_SRS_IOTHUBCLIENT_LL_02_012: [IoTHubClientCore_LL_SendEventAsync shall fail and return IOTHUB_CLIENT_INVALID_ARG if parameter eventConfirmationCallback is NULL and userContextCallback is not NULL.] */
        ((eventConfirmationCallback == NULL) && (userContextCallback != NULL))
        )
    {
        result = IOTHUB_CLIENT_INVALID_ARG;
        LOG_ERROR_RESULT;
    }
    else
    {
        IOTHUB_MESSAGE_LIST *newEntry = (IOTHUB_MESSAGE_LIST*)malloc(sizeof(IOTHUB_MESSAGE_LIST));
        if (newEntry == NULL)
        {
            result = IOTHUB_CLIENT_ERROR;
            LOG_ERROR_RESULT;
        }
        else
        {
            IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;

            if (attach_ms_timesOutAfter(handleData, newEntry) != 0)
            {
                result = IOTHUB_CLIENT_ERROR;
                LOG_ERROR_RESULT;
                free(newEntry);
            }
            else
            {
                /*Codes_SRS_IOTHUBCLIENT_LL_02_013: [IoTHubClientCore_LL_SendEventAsync shall add the DLIST waitingToSend a new record cloning the information from eventMessageHandle, eventConfirmationCallback, userContextCallback.]*/
                if ((newEntry->messageHandle = IoTHubMessage_Clone(eventMessageHandle)) == NULL)
                {
                    result = IOTHUB_CLIENT_ERROR;
                    free(newEntry);
                    LOG_ERROR_RESULT;
                }
                else if (IoTHubClient_Diagnostic_AddIfNecessary(&handleData->diagnostic_setting, newEntry->messageHandle) != 0)
                {
                    /*Codes_SRS_IOTHUBCLIENT_LL_02_014: [If cloning and/or adding the information/diagnostic fails for any reason, IoTHubClientCore_LL_SendEventAsync shall fail and return IOTHUB_CLIENT_ERROR.] */
                    result = IOTHUB_CLIENT_ERROR;
                    IoTHubMessage_Destroy(newEntry->messageHandle);
                    free(newEntry);
                    LOG_ERROR_RESULT;
                }
                else
                {
                    /*Codes_SRS_IOTHUBCLIENT_LL_02_013: [IoTHubClientCore_LL_SendEventAsync shall add the DLIST waitingToSend a new record cloning the information from eventMessageHandle, eventConfirmationCallback, userContextCallback.]*/
                    newEntry->callback = eventConfirmationCallback;
                    newEntry->context = userContextCallback;
                    DList_InsertTailList(&(iotHubClientHandle->waitingToSend), &(newEntry->entry));
                    /*Codes_SRS_IOTHUBCLIENT_LL_02_015: [Otherwise IoTHubClientCore_LL_SendEventAsync shall succeed and return IOTHUB_CLIENT_OK.] */
                    result = IOTHUB_CLIENT_OK;
                }
            }
        }
    }
    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SetMessageCallback(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, IOTHUB_CLIENT_MESSAGE_CALLBACK_ASYNC messageCallback, void* userContextCallback)
{
    IOTHUB_CLIENT_RESULT result;
    if (iotHubClientHandle == NULL)
    {
        /*Codes_SRS_IOTHUBCLIENT_LL_02_016: [IoTHubClientCore_LL_SetMessageCallback shall fail and return IOTHUB_CLIENT_INVALID_ARG if parameter iotHubClientHandle is NULL.] */
        LogError("Invalid argument - iotHubClientHandle is NULL");
        result = IOTHUB_CLIENT_INVALID_ARG;
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;
        if (messageCallback == NULL)
        {
            if (handleData->messageCallback.type == CALLBACK_TYPE_NONE)
            {
                /*Codes_SRS_IOTHUBCLIENT_LL_10_010: [If parameter messageCallback is NULL and the _SetMessageCallback had not been called to subscribe for messages, then IoTHubClientCore_LL_SetMessageCallback shall fail and return IOTHUB_CLIENT_ERROR.] */
                LogError("not currently set to accept or process incoming messages.");
                result = IOTHUB_CLIENT_ERROR;
            }
            else if (handleData->messageCallback.type == CALLBACK_TYPE_ASYNC)
            {
                /*Codes_SRS_IOTHUBCLIENT_LL_10_010: [If parameter messageCallback is NULL and the _SetMessageCallback had not been called to subscribe for messages, then IoTHubClientCore_LL_SetMessageCallback shall fail and return IOTHUB_CLIENT_ERROR.] */
                LogError("Invalid workflow sequence. Please unsubscribe using the IoTHubClientCore_LL_SetMessageCallback_Ex function.");
                result = IOTHUB_CLIENT_ERROR;
            }
            else
            {
                if (handleData->registration_state != OP_STATE_IOT_STAGE)
                {
                    // Remove the subscribe if it's set
                    handleData->iothub_op_init |= ~IOTHUB_OP_C2D_SUB;
                }
                else
                {
                    /*Codes_SRS_IOTHUBCLIENT_LL_02_019: [If parameter messageCallback is NULL then IoTHubClientCore_LL_SetMessageCallback shall call the underlying layer's _Unsubscribe function and return IOTHUB_CLIENT_OK.] */
                    handleData->IoTHubTransport_Unsubscribe(handleData->deviceHandle);
                }
                handleData->messageCallback.type = CALLBACK_TYPE_NONE;
                handleData->messageCallback.callbackSync = NULL;
                handleData->messageCallback.callbackAsync = NULL;
                handleData->messageCallback.userContextCallback = NULL;
                result = IOTHUB_CLIENT_OK;
            }
        }
        else
        {
            if (handleData->messageCallback.type == CALLBACK_TYPE_ASYNC)
            {
                /* Codes_SRS_IOTHUBCLIENT_LL_10_011: [If parameter messageCallback is non-NULL and the _SetMessageCallback_Ex had been used to susbscribe for messages, then IoTHubClientCore_LL_SetMessageCallback shall fail and return IOTHUB_CLIENT_ERROR.] */
                LogError("Invalid workflow sequence. Please unsubscribe using the IoTHubClientCore_LL_SetMessageCallback_Ex function before subscribing with MessageCallback.");
                result = IOTHUB_CLIENT_ERROR;
            }
            else
            {
                if (handleData->registration_state != OP_STATE_IOT_STAGE)
                {
                    // Remove the subscribe if it's set
                    handleData->iothub_op_init |= IOTHUB_OP_C2D_SUB;
                    handleData->messageCallback.type = CALLBACK_TYPE_SYNC;
                    handleData->messageCallback.callbackSync = messageCallback;
                    handleData->messageCallback.userContextCallback = userContextCallback;
                    result = IOTHUB_CLIENT_OK;
                }
                else
                {
                    if (handleData->IoTHubTransport_Subscribe(handleData->deviceHandle) == 0)
                    {
                        /*Codes_SRS_IOTHUBCLIENT_LL_02_017: [If parameter messageCallback is non-NULL then IoTHubClientCore_LL_SetMessageCallback shall call the underlying layer's _Subscribe function.]*/
                        handleData->messageCallback.type = CALLBACK_TYPE_SYNC;
                        handleData->messageCallback.callbackSync = messageCallback;
                        handleData->messageCallback.userContextCallback = userContextCallback;
                        result = IOTHUB_CLIENT_OK;
                    }
                    else
                    {
                        /*Codes_SRS_IOTHUBCLIENT_LL_02_018: [If the underlying layer's _Subscribe function fails, then IoTHubClientCore_LL_SetMessageCallback shall fail and return IOTHUB_CLIENT_ERROR. Otherwise IoTHubClientCore_LL_SetMessageCallback shall succeed and return IOTHUB_CLIENT_OK.]*/
                        LogError("IoTHubTransport_Subscribe failed");
                        handleData->messageCallback.type = CALLBACK_TYPE_NONE;
                        handleData->messageCallback.callbackSync = NULL;
                        handleData->messageCallback.callbackAsync = NULL;
                        handleData->messageCallback.userContextCallback = NULL;
                        result = IOTHUB_CLIENT_ERROR;
                    }
                }
            }
        }
    }
    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SetMessageCallback_Ex(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, IOTHUB_CLIENT_MESSAGE_CALLBACK_ASYNC_EX messageCallback, void* userContextCallback)
{
    IOTHUB_CLIENT_RESULT result;
    if (iotHubClientHandle == NULL)
    {
        /*Codes_SRS_IOTHUBCLIENT_LL_10_021: [IoTHubClientCore_LL_SetMessageCallback_Ex shall fail and return IOTHUB_CLIENT_INVALID_ARG if parameter iotHubClientHandle is NULL.]*/
        LogError("Invalid argument - iotHubClientHandle is NULL");
        result = IOTHUB_CLIENT_INVALID_ARG;
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;
        if (messageCallback == NULL)
        {
            if (handleData->messageCallback.type == CALLBACK_TYPE_NONE)
            {
                /*Codes_SRS_IOTHUBCLIENT_LL_10_018: [If parameter messageCallback is NULL and IoTHubClientCore_LL_SetMessageCallback_Ex had not been used to subscribe for messages, then IoTHubClientCore_LL_SetMessageCallback_Ex shall fail and return IOTHUB_CLIENT_ERROR.] */
                LogError("not currently set to accept or process incoming messages.");
                result = IOTHUB_CLIENT_ERROR;
            }
            else if (handleData->messageCallback.type == CALLBACK_TYPE_SYNC)
            {
                /*Codes_SRS_IOTHUBCLIENT_LL_10_019: [If parameter messageCallback is NULL and IoTHubClientCore_LL_SetMessageCallback had been used to subscribe for messages, then IoTHubClientCore_LL_SetMessageCallback_Ex shall fail and return IOTHUB_CLIENT_ERROR.] */
                LogError("Invalid workflow sequence. Please unsubscribe using the IoTHubClientCore_LL_SetMessageCallback function.");
                result = IOTHUB_CLIENT_ERROR;
            }
            else
            {
                if (handleData->registration_state != OP_STATE_IOT_STAGE)
                {
                    // Remove the subscribe if it's set
                    handleData->iothub_op_init |= ~IOTHUB_OP_C2D_SUB;
                }
                else
                {
                    /*Codes_SRS_IOTHUBCLIENT_LL_10_023: [If parameter messageCallback is NULL then IoTHubClientCore_LL_SetMessageCallback_Ex shall call the underlying layer's _Unsubscribe function and return IOTHUB_CLIENT_OK.] */
                    handleData->IoTHubTransport_Unsubscribe(handleData->deviceHandle);
                }
                handleData->messageCallback.type = CALLBACK_TYPE_NONE;
                handleData->messageCallback.callbackSync = NULL;
                handleData->messageCallback.callbackAsync = NULL;
                handleData->messageCallback.userContextCallback = NULL;
                result = IOTHUB_CLIENT_OK;
            }
        }
        else
        {
            if (handleData->messageCallback.type == CALLBACK_TYPE_SYNC)
            {
                /*Codes_SRS_IOTHUBCLIENT_LL_10_020: [If parameter messageCallback is non-NULL, and IoTHubClientCore_LL_SetMessageCallback had been used to subscribe for messages, then IoTHubClientCore_LL_SetMessageCallback_Ex shall fail and return IOTHUB_CLIENT_ERROR.] */
                LogError("Invalid workflow sequence. Please unsubscribe using the IoTHubClientCore_LL_MessageCallbackEx function before subscribing with MessageCallback.");
                result = IOTHUB_CLIENT_ERROR;
            }
            else
            {
                if (handleData->registration_state != OP_STATE_IOT_STAGE)
                {
                    handleData->iothub_op_init |= IOTHUB_OP_C2D_SUB;
                    handleData->messageCallback.type = CALLBACK_TYPE_ASYNC;
                    handleData->messageCallback.callbackAsync = messageCallback;
                    handleData->messageCallback.userContextCallback = userContextCallback;
                    result = IOTHUB_CLIENT_OK;
                }
                else
                {
                    if (handleData->IoTHubTransport_Subscribe(handleData->deviceHandle) == 0)
                    {
                        /*Codes_SRS_IOTHUBCLIENT_LL_10_024: [If parameter messageCallback is non-NULL then IoTHubClientCore_LL_SetMessageCallback_Ex shall call the underlying layer's _Subscribe function.]*/
                        handleData->messageCallback.type = CALLBACK_TYPE_ASYNC;
                        handleData->messageCallback.callbackAsync = messageCallback;
                        handleData->messageCallback.userContextCallback = userContextCallback;
                        result = IOTHUB_CLIENT_OK;
                    }
                    else
                    {
                        /*Codes_SRS_IOTHUBCLIENT_LL_10_025: [If the underlying layer's _Subscribe function fails, then IoTHubClientCore_LL_SetMessageCallback_Ex shall fail and return IOTHUB_CLIENT_ERROR. Otherwise IoTHubClientCore_LL_SetMessageCallback_Ex shall succeed and return IOTHUB_CLIENT_OK.] */
                        LogError("IoTHubTransport_Subscribe failed");
                        handleData->messageCallback.type = CALLBACK_TYPE_NONE;
                        handleData->messageCallback.callbackSync = NULL;
                        handleData->messageCallback.callbackAsync = NULL;
                        handleData->messageCallback.userContextCallback = NULL;
                        result = IOTHUB_CLIENT_ERROR;
                    }
                }
            }
        }
    }
    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SendMessageDisposition(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, MESSAGE_CALLBACK_INFO* message_data, IOTHUBMESSAGE_DISPOSITION_RESULT disposition)
{
    IOTHUB_CLIENT_RESULT result;
    if ((iotHubClientHandle == NULL) || (message_data == NULL))
    {
        /*Codes_SRS_IOTHUBCLIENT_LL_10_026: [IoTHubClientCore_LL_SendMessageDisposition shall fail and return IOTHUB_CLIENT_INVALID_ARG if parameter iotHubClientHandle is NULL.]*/
        LogError("Invalid argument handle=%p, message=%p", iotHubClientHandle, message_data);
        result = IOTHUB_CLIENT_INVALID_ARG;
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;
        /*Codes_SRS_IOTHUBCLIENT_LL_10_027: [IoTHubClientCore_LL_SendMessageDisposition shall return the result from calling the underlying layer's _Send_Message_Disposition.]*/
        result = handleData->IoTHubTransport_SendMessageDisposition(message_data, disposition);
    }
    return result;
}

void IoTHubClientCore_LL_DoWork(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle)
{
    /*Codes_SRS_IOTHUBCLIENT_LL_02_020: [If parameter iotHubClientHandle is NULL then IoTHubClientCore_LL_DoWork shall not perform any action.] */
    if (iotHubClientHandle != NULL)
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;
#ifdef USE_PROV_MODULE
        switch (handleData->registration_state)
        {
            case OP_STATE_IDLE:
            case OP_STATE_REGISTERING:
            case OP_STATE_REGISTERED:
                process_provisioning_dowork(handleData);
                break;
            case OP_STATE_IOT_STAGE:
            default:
                process_iothub_dowork(handleData);
                break;
        }
#else
        process_iothub_dowork(handleData);
#endif
    }
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_GetSendStatus(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, IOTHUB_CLIENT_STATUS *iotHubClientStatus)
{
    IOTHUB_CLIENT_RESULT result;

    /* Codes_SRS_IOTHUBCLIENT_09_007: [IoTHubClient_GetSendStatus shall return IOTHUB_CLIENT_INVALID_ARG if called with NULL parameter] */
    if (iotHubClientHandle == NULL || iotHubClientStatus == NULL)
    {
        result = IOTHUB_CLIENT_INVALID_ARG;
        LOG_ERROR_RESULT;
    }
    else if (iotHubClientHandle->registration_state != OP_STATE_IOT_STAGE)
    {
        LogError("unable to get send status till device is provisioned");
        result = IOTHUB_CLIENT_PROVISIONING_NOT_COMPLETE;
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;

        /* Codes_SRS_IOTHUBCLIENT_09_008: [IoTHubClient_GetSendStatus shall return IOTHUB_CLIENT_OK and status IOTHUB_CLIENT_SEND_STATUS_IDLE if there is currently no items to be sent] */
        /* Codes_SRS_IOTHUBCLIENT_09_009: [IoTHubClient_GetSendStatus shall return IOTHUB_CLIENT_OK and status IOTHUB_CLIENT_SEND_STATUS_BUSY if there are currently items to be sent] */
        result = handleData->IoTHubTransport_GetSendStatus(handleData->deviceHandle, iotHubClientStatus);
    }

    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SetConnectionStatusCallback(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, IOTHUB_CLIENT_CONNECTION_STATUS_CALLBACK connectionStatusCallback, void * userContextCallback)
{
    IOTHUB_CLIENT_RESULT result;
    /*Codes_SRS_IOTHUBCLIENT_LL_25_111: [IoTHubClientCore_LL_SetConnectionStatusCallback shall return IOTHUB_CLIENT_INVALID_ARG if called with NULL parameter iotHubClientHandle**]** */
    if (iotHubClientHandle == NULL)
    {
        result = IOTHUB_CLIENT_INVALID_ARG;
        LOG_ERROR_RESULT;
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;
        /*Codes_SRS_IOTHUBCLIENT_LL_25_112: [IoTHubClientCore_LL_SetConnectionStatusCallback shall return IOTHUB_CLIENT_OK and save the callback and userContext as a member of the handle.] */
        handleData->conStatusCallback = connectionStatusCallback;
        handleData->conStatusUserContextCallback = userContextCallback;
        result = IOTHUB_CLIENT_OK;
    }

    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SetRetryPolicy(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, IOTHUB_CLIENT_RETRY_POLICY retryPolicy, size_t retryTimeoutLimitInSeconds)
{
    IOTHUB_CLIENT_RESULT result;
    IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;

    /* Codes_SRS_IOTHUBCLIENT_LL_25_116: [If iotHubClientHandle, retryPolicy or retryTimeoutLimitinSeconds is NULL, IoTHubClientCore_LL_GetRetryPolicy shall return IOTHUB_CLIENT_INVALID_ARG]*/
    if (handleData == NULL)
    {
        result = IOTHUB_CLIENT_INVALID_ARG;
        LOG_ERROR_RESULT;
    }
    else if (handleData->transportHandle == NULL)
    {
        result = IOTHUB_CLIENT_ERROR;
        LOG_ERROR_RESULT;
    }
    else
    {
        if (handleData->IoTHubTransport_SetRetryPolicy(handleData->transportHandle, retryPolicy, retryTimeoutLimitInSeconds) != 0)
        {
            result = IOTHUB_CLIENT_ERROR;
            LOG_ERROR_RESULT;
        }
        else
        {
            /* Codes_SRS_IOTHUBCLIENT_LL_25_118: [IoTHubClientCore_LL_SetRetryPolicy shall save connection retry policies specified by the user to retryPolicy in struct IOTHUB_CLIENT_CORE_LL_HANDLE_DATA] */
            /* Codes_SRS_IOTHUBCLIENT_LL_25_119: [IoTHubClientCore_LL_SetRetryPolicy shall save retryTimeoutLimitInSeconds in seconds to retryTimeout in struct IOTHUB_CLIENT_CORE_LL_HANDLE_DATA] */
            handleData->retryPolicy = retryPolicy;
            handleData->retryTimeoutLimitInSeconds = retryTimeoutLimitInSeconds;
            result = IOTHUB_CLIENT_OK;
        }
    }
    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_GetRetryPolicy(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, IOTHUB_CLIENT_RETRY_POLICY* retryPolicy, size_t* retryTimeoutLimitInSeconds)
{
    IOTHUB_CLIENT_RESULT result;

    /* Codes_SRS_IOTHUBCLIENT_LL_09_001: [IoTHubClientCore_LL_GetLastMessageReceiveTime shall return IOTHUB_CLIENT_INVALID_ARG if any of the arguments is NULL] */
    if (iotHubClientHandle == NULL || retryPolicy == NULL || retryTimeoutLimitInSeconds == NULL)
    {
        LogError("Invalid parameter IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle = %p, IOTHUB_CLIENT_RETRY_POLICY* retryPolicy = %p, size_t* retryTimeoutLimitInSeconds = %p", iotHubClientHandle, retryPolicy, retryTimeoutLimitInSeconds);
        result = IOTHUB_CLIENT_INVALID_ARG;
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;

        *retryPolicy = handleData->retryPolicy;
        *retryTimeoutLimitInSeconds = handleData->retryTimeoutLimitInSeconds;
        result = IOTHUB_CLIENT_OK;
    }
    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_GetLastMessageReceiveTime(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, time_t* lastMessageReceiveTime)
{
    IOTHUB_CLIENT_RESULT result;
    IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;

    /* Codes_SRS_IOTHUBCLIENT_LL_09_001: [IoTHubClientCore_LL_GetLastMessageReceiveTime shall return IOTHUB_CLIENT_INVALID_ARG if any of the arguments is NULL] */
    if (handleData == NULL || lastMessageReceiveTime == NULL)
    {
        result = IOTHUB_CLIENT_INVALID_ARG;
        LOG_ERROR_RESULT;
    }
    else
    {
        /* Codes_SRS_IOTHUBCLIENT_LL_09_002: [IoTHubClientCore_LL_GetLastMessageReceiveTime shall return IOTHUB_CLIENT_INDEFINITE_TIME - and not set 'lastMessageReceiveTime' - if it is unable to provide the time for the last commands] */
        if (handleData->lastMessageReceiveTime == INDEFINITE_TIME)
        {
            result = IOTHUB_CLIENT_INDEFINITE_TIME;
            LOG_ERROR_RESULT;
        }
        else
        {
            /* Codes_SRS_IOTHUBCLIENT_LL_09_003: [IoTHubClientCore_LL_GetLastMessageReceiveTime shall return IOTHUB_CLIENT_OK if it wrote in the lastMessageReceiveTime the time when the last command was received] */
            /* Codes_SRS_IOTHUBCLIENT_LL_09_004: [IoTHubClientCore_LL_GetLastMessageReceiveTime shall return lastMessageReceiveTime in localtime] */
            *lastMessageReceiveTime = handleData->lastMessageReceiveTime;
            result = IOTHUB_CLIENT_OK;
        }
    }

    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SetOption(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, const char* optionName, const void* value)
{

    IOTHUB_CLIENT_RESULT result;
    /*Codes_SRS_IOTHUBCLIENT_LL_02_034: [If iotHubClientHandle is NULL then IoTHubClientCore_LL_SetOption shall return IOTHUB_CLIENT_INVALID_ARG.]*/
    /*Codes_SRS_IOTHUBCLIENT_LL_02_035: [If optionName is NULL then IoTHubClientCore_LL_SetOption shall return IOTHUB_CLIENT_INVALID_ARG.] */
    /*Codes_SRS_IOTHUBCLIENT_LL_02_036: [If value is NULL then IoTHubClientCore_LL_SetOption shall return IOTHUB_CLIENT_INVALID_ARG.] */
    if (
        (iotHubClientHandle == NULL) ||
        (optionName == NULL) ||
        (value == NULL)
        )
    {
        result = IOTHUB_CLIENT_INVALID_ARG;
        LogError("invalid argument (NULL)");
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;

        /*Codes_SRS_IOTHUBCLIENT_LL_02_039: [ "messageTimeout" - once IoTHubClientCore_LL_SendEventAsync is called the message shall timeout after value miliseconds. Value is a pointer to a tickcounter_ms_t. ]*/
        if (strcmp(optionName, OPTION_MESSAGE_TIMEOUT) == 0)
        {
            /*this is an option handled by IoTHubClientCore_LL*/
            /*Codes_SRS_IOTHUBCLIENT_LL_02_043: [ Calling IoTHubClientCore_LL_SetOption with value set to "0" shall disable the timeout mechanism for all new messages. ]*/
            handleData->currentMessageTimeout = *(const tickcounter_ms_t*)value;
            result = IOTHUB_CLIENT_OK;
        }
        else if (strcmp(optionName, OPTION_PRODUCT_INFO) == 0)
        {
            /*Codes_SRS_IOTHUBCLIENT_LL_10_033: [repeat calls with "product_info" will erase the previously set product information if applicatble. ]*/
            if (handleData->product_info != NULL)
            {
                STRING_delete(handleData->product_info);
                handleData->product_info = NULL;
            }

            PLATFORM_INFO_OPTION supportedPlatformInfo;
            if (handleData->IoTHubTransport_GetSupportedPlatformInfo(handleData->transportHandle, &supportedPlatformInfo) != 0)
            {
                LogError("IoTHubTransport_GetSupportedPlatformInfo failed");
                result = IOTHUB_CLIENT_ERROR;
            }
            /*Codes_SRS_IOTHUBCLIENT_LL_10_035: [If string concatenation fails, `IoTHubClientCore_LL_SetOption` shall return `IOTHUB_CLIENT_ERROR`. Otherwise, `IOTHUB_CLIENT_OK` shall be returned. ]*/
            else if ((handleData->product_info = make_product_info((const char*)value, supportedPlatformInfo)) == NULL)
            {
                LogError("STRING_construct_sprintf failed");
                result = IOTHUB_CLIENT_ERROR;
            }
            else
            {
                result = IOTHUB_CLIENT_OK;
            }
        }
        else if (strcmp(optionName, OPTION_DIAGNOSTIC_SAMPLING_PERCENTAGE) == 0)
        {
            uint32_t percentage = *(uint32_t*)value;
            if (percentage > 100)
            {
                /*Codes_SRS_IOTHUBCLIENT_LL_10_036: [Calling IoTHubClientCore_LL_SetOption with value > 100 shall return `IOTHUB_CLIENT_ERRROR`. ]*/
                LogError("The value of diag_sampling_percentage is out of range [0, 100]: %u", percentage);
                result = IOTHUB_CLIENT_ERROR;
            }
            else
            {
                /*Codes_SRS_IOTHUBCLIENT_LL_10_037: [Calling IoTHubClientCore_LL_SetOption with value between [0, 100] shall return `IOTHUB_CLIENT_OK`. ]*/
                handleData->diagnostic_setting.diagSamplingPercentage = percentage;
                handleData->diagnostic_setting.currentMessageNumber = 0;
                result = IOTHUB_CLIENT_OK;
            }
        }
        else if ((strcmp(optionName, OPTION_BLOB_UPLOAD_TIMEOUT_SECS) == 0) || (strcmp(optionName, OPTION_CURL_VERBOSE) == 0))
        {
#ifndef DONT_USE_UPLOADTOBLOB
            result = IOTHUB_CLIENT_OK;
            if (handleData->uploadToBlobHandle == NULL && create_blob_upload_module(handleData) != 0)
            {
                LogError("Failure creating upload to blob object");
                result = IOTHUB_CLIENT_ERROR;
            }
            else if (result == IOTHUB_CLIENT_OK)
            {
                // This option just gets passed down into IoTHubClientCore_LL_UploadToBlob
                /*Codes_SRS_IOTHUBCLIENT_LL_30_010: [ blob_xfr_timeout - IoTHubClientCore_LL_SetOption shall pass this option to IoTHubClient_UploadToBlob_SetOption and return its result. ]*/
                result = IoTHubClient_LL_UploadToBlob_SetOption(handleData->uploadToBlobHandle, optionName, value);
                if(result != IOTHUB_CLIENT_OK)
                {
                    LogError("unable to IoTHubClientCore_LL_UploadToBlob_SetOption, result=%d", result);
                }
            }
#else
            LogError("%s option being set with DONT_USE_UPLOADTOBLOB compiler switch", optionName);
            result = IOTHUB_CLIENT_ERROR;
#endif /*DONT_USE_UPLOADTOBLOB*/
        }
        // OPTION_SAS_TOKEN_REFRESH_TIME is, but may be updated in the future
        // if this becomes necessary
        else if (strcmp(optionName, OPTION_SAS_TOKEN_REFRESH_TIME) == 0 || strcmp(optionName, OPTION_SAS_TOKEN_LIFETIME) == 0)
        {
            if (IoTHubClient_Auth_Set_SasToken_Expiry(handleData->authorization_module, *(size_t*)value) != 0)
            {
                LogError("Failed setting the Token Expiry time");
                result = IOTHUB_CLIENT_ERROR;
            }
            else
            {
                result = IOTHUB_CLIENT_OK;
            }
        }
        else if (strcmp(optionName, OPTION_LOG_TRACE) == 0)
        {
            handleData->log_trace = *((bool*)value);
            if (handleData->registration_state == OP_STATE_IOT_STAGE)
            {
                result = handleData->IoTHubTransport_SetOption(handleData->transportHandle, optionName, value);
                if (result != IOTHUB_CLIENT_OK)
                {
                    LogError("unable to IoTHubTransport_SetOption");
                }
                else
                {
                    result = IOTHUB_CLIENT_OK;
                }
            }
#ifdef USE_PROV_MODULE
            else
            {
                // We need to capture log trace and then pass it down to the lower layer
                if (Prov_Device_LL_SetOption(handleData->prov_handle, optionName, value) != PROV_DEVICE_RESULT_OK)
                {
                    LogError("Failure setting log trace in provisioning");
                    result = IOTHUB_CLIENT_ERROR;
                }
                else
                {
                    result = IOTHUB_CLIENT_OK;
                }
            }
#else
            else
            {
                result = IOTHUB_CLIENT_OK;
            }
#endif
        }
        else
        {
            // This section is unusual for SetOption calls because it attempts to pass unhandled options
            // to two downstream targets (IoTHubTransport_SetOption and IoTHubClientCore_LL_UploadToBlob_SetOption) instead of one.

            /*Codes_SRS_IOTHUBCLIENT_LL_30_011: [ IoTHubClientCore_LL_SetOption shall always pass unhandled options to Transport_SetOption. ]*/
            /*Codes_SRS_IOTHUBCLIENT_LL_30_012: [ If Transport_SetOption fails, IoTHubClientCore_LL_SetOption shall return that failure code. ]*/
            result = handleData->IoTHubTransport_SetOption(handleData->transportHandle, optionName, value);
            if(result != IOTHUB_CLIENT_OK)
            {
                LogError("unable to IoTHubTransport_SetOption");
            }
#ifndef DONT_USE_UPLOADTOBLOB
            else
            {
                /*Codes_SRS_IOTHUBCLIENT_LL_30_013: [ If the DONT_USE_UPLOADTOBLOB compiler switch is undefined, IoTHubClientCore_LL_SetOption shall pass unhandled options to IoTHubClient_UploadToBlob_SetOption and ignore the result. ]*/
                (void)IoTHubClient_LL_UploadToBlob_SetOption(handleData->uploadToBlobHandle, optionName, value);
            }
#endif /*DONT_USE_UPLOADTOBLOB*/
        }
    }
    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SetDeviceTwinCallback(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, IOTHUB_CLIENT_DEVICE_TWIN_CALLBACK deviceTwinCallback, void* userContextCallback)
{
    IOTHUB_CLIENT_RESULT result;
    /* Codes_SRS_IOTHUBCLIENT_LL_10_001: [ IoTHubClientCore_LL_SetDeviceTwinCallback shall fail and return IOTHUB_CLIENT_INVALID_ARG if parameter iotHubClientHandle is NULL.] */
    if (iotHubClientHandle == NULL)
    {
        result = IOTHUB_CLIENT_INVALID_ARG;
        LogError("Invalid argument specified iothubClientHandle=%p", iotHubClientHandle);
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;
        if (deviceTwinCallback == NULL)
        {
            if (handleData->registration_state != OP_STATE_IOT_STAGE)
            {
                // Remove the subscribe if it's set
                handleData->iothub_op_init &= ~IOTHUB_OP_DEVICE_TWIN_SUB;
            }
            else
            {
                /* Codes_SRS_IOTHUBCLIENT_LL_10_006: [ If deviceTwinCallback is NULL, then IoTHubClientCore_LL_SetDeviceTwinCallback shall call the underlying layer's _Unsubscribe function and return IOTHUB_CLIENT_OK.] */
                handleData->IoTHubTransport_Unsubscribe_DeviceTwin(handleData->transportHandle);
            }
            handleData->deviceTwinCallback = NULL;
            result = IOTHUB_CLIENT_OK;
        }
        else
        {
            if (handleData->registration_state != OP_STATE_IOT_STAGE)
            {
                // Remove the subscribe if it's set
                handleData->iothub_op_init |= IOTHUB_OP_DEVICE_TWIN_SUB;
                handleData->deviceTwinCallback = deviceTwinCallback;
                handleData->deviceTwinContextCallback = userContextCallback;
                result = IOTHUB_CLIENT_OK;
            }
            else
            {
                /* Codes_SRS_IOTHUBCLIENT_LL_10_002: [ If deviceTwinCallback is not NULL, then IoTHubClientCore_LL_SetDeviceTwinCallback shall call the underlying layer's _Subscribe function.] */
                if (handleData->IoTHubTransport_Subscribe_DeviceTwin(handleData->transportHandle) == 0)
                {
                    handleData->deviceTwinCallback = deviceTwinCallback;
                    handleData->deviceTwinContextCallback = userContextCallback;
                    /* Codes_SRS_IOTHUBCLIENT_LL_10_005: [ Otherwise IoTHubClientCore_LL_SetDeviceTwinCallback shall succeed and return IOTHUB_CLIENT_OK.] */
                    result = IOTHUB_CLIENT_OK;
                }
                else
                {
                    /* Codes_SRS_IOTHUBCLIENT_LL_10_003: [ If the underlying layer's _Subscribe function fails, then IoTHubClientCore_LL_SetDeviceTwinCallback shall fail and return IOTHUB_CLIENT_ERROR.] */
                    result = IOTHUB_CLIENT_ERROR;
                }
            }
        }
    }
    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SendReportedState(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, const unsigned char* reportedState, size_t size, IOTHUB_CLIENT_REPORTED_STATE_CALLBACK reportedStateCallback, void* userContextCallback)
{
    IOTHUB_CLIENT_RESULT result;
    /* Codes_SRS_IOTHUBCLIENT_LL_10_012: [ IoTHubClientCore_LL_SendReportedState shall fail and return IOTHUB_CLIENT_INVALID_ARG if parameter iotHubClientHandle is NULL. ] */
    /* Codes_SRS_IOTHUBCLIENT_LL_10_013: [ IoTHubClientCore_LL_SendReportedState shall fail and return IOTHUB_CLIENT_INVALID_ARG if parameter reportedState is NULL] */
    /* Codes_SRS_IOTHUBCLIENT_LL_07_005: [ IoTHubClientCore_LL_SendReportedState shall fail and return IOTHUB_CLIENT_INVALID_ARG if parameter size is equal to 0. ] */
    if (iotHubClientHandle == NULL || (reportedState == NULL || size == 0) )
    {
        result = IOTHUB_CLIENT_INVALID_ARG;
        LogError("Invalid argument specified iothubClientHandle=%p, reportedState=%p, size=%lu", iotHubClientHandle, reportedState, (unsigned long)size);
    }
    else if (iotHubClientHandle->registration_state != OP_STATE_IOT_STAGE)
    {
        LogError("unable to send reported state till device is provisioned");
        result = IOTHUB_CLIENT_PROVISIONING_NOT_COMPLETE;
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;
        /* Codes_SRS_IOTHUBCLIENT_LL_10_014: [IoTHubClientCore_LL_SendReportedState shall construct and queue the reported a Device_Twin structure for transmition by the underlying transport.] */
        IOTHUB_DEVICE_TWIN* client_data = dev_twin_data_create(handleData, get_next_item_id(handleData), reportedState, size, reportedStateCallback, userContextCallback);
        if (client_data == NULL)
        {
            /* Codes_SRS_IOTHUBCLIENT_LL_10_015: [If any error is encountered IoTHubClientCore_LL_SendReportedState shall return IOTHUB_CLIENT_ERROR.] */
            LogError("Failure constructing device twin data");
            result = IOTHUB_CLIENT_ERROR;
        }
        else
        {
            if (handleData->IoTHubTransport_Subscribe_DeviceTwin(handleData->transportHandle) != 0)
            {
                LogError("Failure adding device twin data to queue");
                device_twin_data_destroy(client_data);
                result = IOTHUB_CLIENT_ERROR;
            }
            else
            {
                /* Codes_SRS_IOTHUBCLIENT_LL_07_001: [ IoTHubClientCore_LL_SendReportedState shall queue the constructed reportedState data to be consumed by the targeted transport. ] */
                DList_InsertTailList(&(iotHubClientHandle->iot_msg_queue), &(client_data->entry));

                /* Codes_SRS_IOTHUBCLIENT_LL_10_016: [ Otherwise IoTHubClientCore_LL_SendReportedState shall succeed and return IOTHUB_CLIENT_OK.] */
                result = IOTHUB_CLIENT_OK;
            }
        }
    }
    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_GetTwinAsync(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, IOTHUB_CLIENT_DEVICE_TWIN_CALLBACK deviceTwinCallback, void* userContextCallback)
{
    IOTHUB_CLIENT_RESULT result;

    // Codes_SRS_IOTHUBCLIENT_LL_09_011: [ If `iotHubClientHandle` or `deviceTwinCallback` are `NULL`, `IoTHubClientCore_LL_GetTwinAsync` shall fail and return `IOTHUB_CLIENT_INVALID_ARG`. ]
    if (iotHubClientHandle == NULL || deviceTwinCallback == NULL)
    {
        LogError("Invalid argument iothubClientHandle=%p, deviceTwinCallback=%p", iotHubClientHandle, deviceTwinCallback);
        result = IOTHUB_CLIENT_INVALID_ARG;
    }
    else if (iotHubClientHandle->registration_state != OP_STATE_IOT_STAGE)
    {
        LogError("unable to get twin update till device is provisioned");
        result = IOTHUB_CLIENT_PROVISIONING_NOT_COMPLETE;
    }
    else
    {
        if (iotHubClientHandle->IoTHubTransport_Subscribe_DeviceTwin(iotHubClientHandle->transportHandle) != 0)
        {
            LogError("Failure adding device twin data to queue");
            result = IOTHUB_CLIENT_ERROR;
        }
        else
        {
            GET_TWIN_CONTEXT* getTwinCtx;

            if ((getTwinCtx = (GET_TWIN_CONTEXT*)malloc(sizeof(GET_TWIN_CONTEXT))) == NULL)
            {
                LogError("Failed creating get-twin context");
                result = IOTHUB_CLIENT_ERROR;
            }
            else
            {
                IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;

                getTwinCtx->callback = deviceTwinCallback;
                getTwinCtx->context = userContextCallback;

                // Codes_SRS_IOTHUBCLIENT_LL_09_012: [ IoTHubClientCore_LL_GetTwinAsync shall invoke IoTHubTransport_GetTwinAsync, passing `on_device_twin_report_received` and the user data as context  ]
                if (handleData->IoTHubTransport_GetTwinAsync(handleData->deviceHandle, on_get_device_twin_completed, getTwinCtx) != IOTHUB_CLIENT_OK)
                {
                    // Codes_SRS_IOTHUBCLIENT_LL_09_013: [ If IoTHubTransport_GetTwinAsync fails, `IoTHubClientCore_LL_GetTwinAsync` shall fail and return `IOTHUB_CLIENT_ERROR`. ]
                    LogError("Failed getting device twin document");
                    free(getTwinCtx);
                    result = IOTHUB_CLIENT_ERROR;
                }
                else
                {
                    // Codes_SRS_IOTHUBCLIENT_LL_09_014: [ If no errors occur IoTHubClientCore_LL_GetTwinAsync shall return `IOTHUB_CLIENT_OK`. ]
                    handleData->complete_twin_update_encountered = true;
                    result = IOTHUB_CLIENT_OK;
                }
            }
        }
    }

    return result;
}


IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SetDeviceMethodCallback(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, IOTHUB_CLIENT_DEVICE_METHOD_CALLBACK_ASYNC deviceMethodCallback, void* userContextCallback)
{
    IOTHUB_CLIENT_RESULT result;

    /*Codes_SRS_IOTHUBCLIENT_LL_12_017: [ IoTHubClientCore_LL_SetDeviceMethodCallback shall fail and return IOTHUB_CLIENT_INVALID_ARG if parameter iotHubClientHandle is NULL. ] */
    if (iotHubClientHandle == NULL)
    {
        result = IOTHUB_CLIENT_INVALID_ARG;
        LOG_ERROR_RESULT;
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;
        if (deviceMethodCallback == NULL)
        {
            if (handleData->methodCallback.type == CALLBACK_TYPE_NONE)
            {
                /* Codes_SRS_IOTHUBCLIENT_LL_10_029: [ If deviceMethodCallback is NULL and the client is not subscribed to receive method calls, IoTHubClientCore_LL_SetDeviceMethodCallback shall fail and return IOTHUB_CLIENT_ERROR. ] */
                LogError("not currently set to accept or process incoming messages.");
                result = IOTHUB_CLIENT_ERROR;
            }
            else if (handleData->methodCallback.type == CALLBACK_TYPE_ASYNC)
            {
                /* Codes_SRS_IOTHUBCLIENT_LL_10_028: [If the user has subscribed using IoTHubClientCore_LL_SetDeviceMethodCallback_Ex, IoTHubClientCore_LL_SetDeviceMethodCallback shall fail and return IOTHUB_CLIENT_ERROR. ] */
                LogError("Invalid workflow sequence. Please unsubscribe using the IoTHubClientCore_LL_SetDeviceMethodCallback_Ex function.");
                result = IOTHUB_CLIENT_ERROR;
            }
            else if (handleData->registration_state != OP_STATE_IOT_STAGE)
            {
                // Remove the subscribe if it's set
                handleData->iothub_op_init &= ~IOTHUB_OP_METHOD_SUB;
                result = IOTHUB_CLIENT_OK;
            }
            else
            {
                /*Codes_SRS_IOTHUBCLIENT_LL_02_019: [If parameter messageCallback is NULL then IoTHubClientCore_LL_SetMessageCallback shall call the underlying layer's _Unsubscribe function and return IOTHUB_CLIENT_OK.] */
                /*Codes_SRS_IOTHUBCLIENT_LL_12_018: [If deviceMethodCallback is NULL, then IoTHubClientCore_LL_SetDeviceMethodCallback shall call the underlying layer's IoTHubTransport_Unsubscribe_DeviceMethod function and return IOTHUB_CLIENT_OK. ] */
                /*Codes_SRS_IOTHUBCLIENT_LL_12_022: [ Otherwise IoTHubClientCore_LL_SetDeviceMethodCallback shall succeed and return IOTHUB_CLIENT_OK. ]*/
                handleData->IoTHubTransport_Unsubscribe_DeviceMethod(handleData->deviceHandle);
                result = IOTHUB_CLIENT_OK;
            }
            handleData->methodCallback.type = CALLBACK_TYPE_NONE;
            handleData->methodCallback.callbackSync = NULL;
            handleData->methodCallback.userContextCallback = NULL;
        }
        else
        {
            if (handleData->methodCallback.type == CALLBACK_TYPE_ASYNC)
            {
                /* Codes_SRS_IOTHUBCLIENT_LL_10_028: [If the user has subscribed using IoTHubClientCore_LL_SetDeviceMethodCallback_Ex, IoTHubClientCore_LL_SetDeviceMethodCallback shall fail and return IOTHUB_CLIENT_ERROR. ] */
                LogError("Invalid workflow sequence. Please unsubscribe using the IoTHubClientCore_LL_SetDeviceMethodCallback_Ex function before subscribing with IoTHubClientCore_LL_SetDeviceMethodCallback.");
                result = IOTHUB_CLIENT_ERROR;
            }
            else
            {
                if (handleData->registration_state != OP_STATE_IOT_STAGE)
                {
                    handleData->iothub_op_init |= IOTHUB_OP_METHOD_SUB;
                    handleData->methodCallback.type = CALLBACK_TYPE_SYNC;
                    handleData->methodCallback.callbackSync = deviceMethodCallback;
                    handleData->methodCallback.callbackAsync = NULL;
                    handleData->methodCallback.userContextCallback = userContextCallback;
                    result = IOTHUB_CLIENT_OK;
                }
                else
                {
                    /*Codes_SRS_IOTHUBCLIENT_LL_12_019: [ If deviceMethodCallback is not NULL, then IoTHubClientCore_LL_SetDeviceMethodCallback shall call the underlying layer's IoTHubTransport_Subscribe_DeviceMethod function. ]*/
                    if (handleData->IoTHubTransport_Subscribe_DeviceMethod(handleData->deviceHandle) == 0)
                    {
                        /*Codes_SRS_IOTHUBCLIENT_LL_12_022: [ Otherwise IoTHubClientCore_LL_SetDeviceMethodCallback shall succeed and return IOTHUB_CLIENT_OK. ]*/
                        handleData->methodCallback.type = CALLBACK_TYPE_SYNC;
                        handleData->methodCallback.callbackSync = deviceMethodCallback;
                        handleData->methodCallback.callbackAsync = NULL;
                        handleData->methodCallback.userContextCallback = userContextCallback;
                        result = IOTHUB_CLIENT_OK;
                    }
                    else
                    {
                        /*Codes_SRS_IOTHUBCLIENT_LL_12_020: [ If the underlying layer's IoTHubTransport_Subscribe_DeviceMethod function fails, then IoTHubClientCore_LL_SetDeviceMethodCallback shall fail and return IOTHUB_CLIENT_ERROR. ]*/
                        /*Codes_SRS_IOTHUBCLIENT_LL_12_021: [ If adding the information fails for any reason, IoTHubClientCore_LL_SetDeviceMethodCallback shall fail and return IOTHUB_CLIENT_ERROR. ]*/
                        LogError("IoTHubTransport_Subscribe_DeviceMethod failed");
                        handleData->methodCallback.type = CALLBACK_TYPE_NONE;
                        handleData->methodCallback.callbackAsync = NULL;
                        handleData->methodCallback.callbackSync = NULL;
                        handleData->methodCallback.userContextCallback = NULL;
                        result = IOTHUB_CLIENT_ERROR;
                    }
                }
            }
        }
    }
    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SetDeviceMethodCallback_Ex(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, IOTHUB_CLIENT_INBOUND_DEVICE_METHOD_CALLBACK inboundDeviceMethodCallback, void* userContextCallback)
{
    IOTHUB_CLIENT_RESULT result;
    /* Codes_SRS_IOTHUBCLIENT_LL_07_021: [ If handle is NULL then IoTHubClientCore_LL_SetDeviceMethodCallback_Ex shall return IOTHUB_CLIENT_INVALID_ARG.] */
    if (iotHubClientHandle == NULL)
    {
        result = IOTHUB_CLIENT_INVALID_ARG;
        LOG_ERROR_RESULT;
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;
        if (inboundDeviceMethodCallback == NULL)
        {
            if (handleData->methodCallback.type == CALLBACK_TYPE_NONE)
            {
                /* Codes_SRS_IOTHUBCLIENT_LL_10_030: [ If deviceMethodCallback is NULL and the client is not subscribed to receive method calls, IoTHubClientCore_LL_SetDeviceMethodCallback shall fail and return IOTHUB_CLIENT_ERROR. ] */
                LogError("not currently set to accept or process incoming messages.");
                result = IOTHUB_CLIENT_ERROR;
            }
            else if (handleData->methodCallback.type == CALLBACK_TYPE_SYNC)
            {
                /* Codes_SRS_IOTHUBCLIENT_LL_10_031: [If the user has subscribed using IoTHubClientCore_LL_SetDeviceMethodCallback, IoTHubClientCore_LL_SetDeviceMethodCallback_Ex shall fail and return IOTHUB_CLIENT_ERROR. ] */
                LogError("Invalid workflow sequence. Please unsubscribe using the IoTHubClientCore_LL_SetDeviceMethodCallback function.");
                result = IOTHUB_CLIENT_ERROR;
            }
            else
            {
                /* Codes_SRS_IOTHUBCLIENT_LL_07_022: [ If inboundDeviceMethodCallback is NULL then IoTHubClientCore_LL_SetDeviceMethodCallback_Ex shall call the underlying layer's IoTHubTransport_Unsubscribe_DeviceMethod function and return IOTHUB_CLIENT_OK.] */
                handleData->IoTHubTransport_Unsubscribe_DeviceMethod(handleData->deviceHandle);
                handleData->methodCallback.type = CALLBACK_TYPE_NONE;
                handleData->methodCallback.callbackAsync = NULL;
                handleData->methodCallback.userContextCallback = NULL;
                result = IOTHUB_CLIENT_OK;
            }
        }
        else
        {
            if (handleData->methodCallback.type == CALLBACK_TYPE_SYNC)
            {
                /* Codes_SRS_IOTHUBCLIENT_LL_10_031: [If the user has subscribed using IoTHubClientCore_LL_SetDeviceMethodCallback, IoTHubClientCore_LL_SetDeviceMethodCallback_Ex shall fail and return IOTHUB_CLIENT_ERROR. ] */
                LogError("Invalid workflow sequence. Please unsubscribe using the IoTHubClientCore_LL_SetDeviceMethodCallback function before subscribing with IoTHubClientCore_LL_SetDeviceMethodCallback_Ex.");
                result = IOTHUB_CLIENT_ERROR;
            }
            else
            {
                /* Codes_SRS_IOTHUBCLIENT_LL_07_023: [ If inboundDeviceMethodCallback is non-NULL then IoTHubClientCore_LL_SetDeviceMethodCallback_Ex shall call the underlying layer's IoTHubTransport_Subscribe_DeviceMethod function.]*/
                if (handleData->IoTHubTransport_Subscribe_DeviceMethod(handleData->deviceHandle) == 0)
                {
                    handleData->methodCallback.type = CALLBACK_TYPE_ASYNC;
                    handleData->methodCallback.callbackAsync = inboundDeviceMethodCallback;
                    handleData->methodCallback.callbackSync = NULL;
                    handleData->methodCallback.userContextCallback = userContextCallback;
                    result = IOTHUB_CLIENT_OK;
                }
                else
                {
                    /* Codes_SRS_IOTHUBCLIENT_LL_07_025: [ If any error is encountered then IoTHubClientCore_LL_SetDeviceMethodCallback_Ex shall return IOTHUB_CLIENT_ERROR.] */
                    LogError("IoTHubTransport_Subscribe_DeviceMethod failed");
                    handleData->methodCallback.type = CALLBACK_TYPE_NONE;
                    handleData->methodCallback.callbackAsync = NULL;
                    handleData->methodCallback.callbackSync = NULL;
                    handleData->methodCallback.userContextCallback = NULL;
                    result = IOTHUB_CLIENT_ERROR;
                }
            }
        }
    }
    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_DeviceMethodResponse(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, METHOD_HANDLE methodId, const unsigned char* response, size_t response_size, int status_response)
{
    IOTHUB_CLIENT_RESULT result;
    /* Codes_SRS_IOTHUBCLIENT_LL_07_026: [ If handle or methodId is NULL then IoTHubClientCore_LL_DeviceMethodResponse shall return IOTHUB_CLIENT_INVALID_ARG.] */
    if (iotHubClientHandle == NULL || methodId == NULL)
    {
        result = IOTHUB_CLIENT_INVALID_ARG;
        LOG_ERROR_RESULT;
    }
    else if (iotHubClientHandle->registration_state != OP_STATE_IOT_STAGE)
    {
        LogError("unable to get send status till device is provisioned");
        result = IOTHUB_CLIENT_PROVISIONING_NOT_COMPLETE;
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;
        /* Codes_SRS_IOTHUBCLIENT_LL_07_027: [ IoTHubClientCore_LL_DeviceMethodResponse shall call the IoTHubTransport_DeviceMethod_Response transport function.] */
        if (handleData->IoTHubTransport_DeviceMethod_Response(handleData->deviceHandle, methodId, response, response_size, status_response) != 0)
        {
            LogError("IoTHubTransport_DeviceMethod_Response failed");
            result = IOTHUB_CLIENT_ERROR;
        }
        else
        {
            result = IOTHUB_CLIENT_OK;
        }
    }
    return result;
}

#ifndef DONT_USE_UPLOADTOBLOB
IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_UploadToBlob(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, const char* destinationFileName, const unsigned char* source, size_t size)
{
    IOTHUB_CLIENT_RESULT result;
    /*Codes_SRS_IOTHUBCLIENT_LL_02_061: [ If iotHubClientHandle is NULL then IoTHubClientCore_LL_UploadToBlob shall fail and return IOTHUB_CLIENT_INVALID_ARG. ]*/
    /*Codes_SRS_IOTHUBCLIENT_LL_02_062: [ If destinationFileName is NULL then IoTHubClientCore_LL_UploadToBlob shall fail and return IOTHUB_CLIENT_INVALID_ARG. ]*/
    /*Codes_SRS_IOTHUBCLIENT_LL_02_063: [ If `source` is `NULL` and size is greater than 0 then `IoTHubClientCore_LL_UploadToBlob` shall fail and return `IOTHUB_CLIENT_INVALID_ARG`. ]*/
    if (iotHubClientHandle == NULL || destinationFileName == NULL || ((source == NULL) && (size > 0)) )
    {
        LogError("invalid parameters IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle=%p, const char* destinationFileName=%s, const unsigned char* source=%p, size_t size=%lu", iotHubClientHandle, destinationFileName, source, (unsigned long)size);
        result = IOTHUB_CLIENT_INVALID_ARG;
    }
    else if (iotHubClientHandle->uploadToBlobHandle == NULL && create_blob_upload_module(iotHubClientHandle) != 0)
    {
        LogError("Failure creating blob upload handle");
        result = IOTHUB_CLIENT_ERROR;
    }
    else
    {
        result = IoTHubClient_LL_UploadToBlob_Impl(iotHubClientHandle->uploadToBlobHandle, destinationFileName, source, size);
    }
    return result;
}

typedef struct UPLOAD_MULTIPLE_BLOCKS_WRAPPER_CONTEXT_TAG
{
    IOTHUB_CLIENT_FILE_UPLOAD_GET_DATA_CALLBACK getDataCallback;
    void* context;
} UPLOAD_MULTIPLE_BLOCKS_WRAPPER_CONTEXT;


static IOTHUB_CLIENT_FILE_UPLOAD_GET_DATA_RESULT uploadMultipleBlocksCallbackWrapper(IOTHUB_CLIENT_FILE_UPLOAD_RESULT result, unsigned char const ** data, size_t* size, void* context)
{
    UPLOAD_MULTIPLE_BLOCKS_WRAPPER_CONTEXT* wrapperContext = (UPLOAD_MULTIPLE_BLOCKS_WRAPPER_CONTEXT*)context;
    wrapperContext->getDataCallback(result, data, size, wrapperContext->context);
    return IOTHUB_CLIENT_FILE_UPLOAD_GET_DATA_OK;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_UploadMultipleBlocksToBlob(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, const char* destinationFileName, IOTHUB_CLIENT_FILE_UPLOAD_GET_DATA_CALLBACK getDataCallback, void* context)
{
    IOTHUB_CLIENT_RESULT result;
    /*Codes_SRS_IOTHUBCLIENT_LL_99_005: [ If `iotHubClientHandle` is `NULL` then `IoTHubClientCore_LL_UploadMultipleBlocksToBlob(Ex)` shall fail and return `IOTHUB_CLIENT_INVALID_ARG`. ]*/
    /*Codes_SRS_IOTHUBCLIENT_LL_99_006: [ If `destinationFileName` is `NULL` then `IoTHubClientCore_LL_UploadMultipleBlocksToBlob(Ex)` shall fail and return `IOTHUB_CLIENT_INVALID_ARG`. ]*/
    /*Codes_SRS_IOTHUBCLIENT_LL_99_007: [ If `getDataCallback` is `NULL` then `IoTHubClientCore_LL_UploadMultipleBlocksToBlob(Ex)` shall fail and return `IOTHUB_CLIENT_INVALID_ARG`. ]*/
    if (
        (iotHubClientHandle == NULL) ||
        (destinationFileName == NULL) ||
        (getDataCallback == NULL)
        )
    {
        LogError("invalid parameters IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle=%p, const char* destinationFileName=%p, getDataCallback=%p", iotHubClientHandle, destinationFileName, getDataCallback);
        result = IOTHUB_CLIENT_INVALID_ARG;
    }
    else
    {
        UPLOAD_MULTIPLE_BLOCKS_WRAPPER_CONTEXT uploadMultipleBlocksWrapperContext;
        uploadMultipleBlocksWrapperContext.getDataCallback = getDataCallback;
        uploadMultipleBlocksWrapperContext.context = context;

        result = IoTHubClient_LL_UploadMultipleBlocksToBlob_Impl(iotHubClientHandle->uploadToBlobHandle, destinationFileName, uploadMultipleBlocksCallbackWrapper, &uploadMultipleBlocksWrapperContext);
    }
    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_UploadMultipleBlocksToBlobEx(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, const char* destinationFileName, IOTHUB_CLIENT_FILE_UPLOAD_GET_DATA_CALLBACK_EX getDataCallbackEx, void* context)
{
    IOTHUB_CLIENT_RESULT result;
    /*Codes_SRS_IOTHUBCLIENT_LL_99_005: [ If `iotHubClientHandle` is `NULL` then `IoTHubClientCore_LL_UploadMultipleBlocksToBlob(Ex)` shall fail and return `IOTHUB_CLIENT_INVALID_ARG`. ]*/
    /*Codes_SRS_IOTHUBCLIENT_LL_99_006: [ If `destinationFileName` is `NULL` then `IoTHubClientCore_LL_UploadMultipleBlocksToBlob(Ex)` shall fail and return `IOTHUB_CLIENT_INVALID_ARG`. ]*/
    /*Codes_SRS_IOTHUBCLIENT_LL_99_007: [ If `getDataCallback` is `NULL` then `IoTHubClientCore_LL_UploadMultipleBlocksToBlob(Ex)` shall fail and return `IOTHUB_CLIENT_INVALID_ARG`. ]*/
    if (
        (iotHubClientHandle == NULL) ||
        (destinationFileName == NULL) ||
        (getDataCallbackEx == NULL)
        )
    {
        LogError("invalid parameters IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle=%p, destinationFileName=%p, getDataCallbackEx=%p", iotHubClientHandle, destinationFileName, getDataCallbackEx);
        result = IOTHUB_CLIENT_INVALID_ARG;
    }
    else
    {
        if (iotHubClientHandle->uploadToBlobHandle == NULL && create_blob_upload_module(iotHubClientHandle) != 0)
        {
            LogError("Failure creating upload module");
            result = IOTHUB_CLIENT_ERROR;
        }
        else
        {
            result = IoTHubClient_LL_UploadMultipleBlocksToBlob_Impl(iotHubClientHandle->uploadToBlobHandle, destinationFileName, getDataCallbackEx, context);
        }
    }
    return result;
}
#endif // DONT_USE_UPLOADTOBLOB

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SendEventToOutputAsync(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, IOTHUB_MESSAGE_HANDLE eventMessageHandle, const char* outputName, IOTHUB_CLIENT_EVENT_CONFIRMATION_CALLBACK eventConfirmationCallback, void* userContextCallback)
{
    IOTHUB_CLIENT_RESULT result;

    if ((iotHubClientHandle == NULL) || (outputName == NULL) || (eventMessageHandle == NULL) || ((eventConfirmationCallback == NULL) && (userContextCallback != NULL)))
    {
        // Codes_SRS_IOTHUBCLIENT_LL_31_127: [ If `iotHubClientHandle`, `outputName`, or `eventConfirmationCallback` is `NULL`, `IoTHubClient_LL_SendEventToOutputAsync` shall return `IOTHUB_CLIENT_INVALID_ARG`. ]
        LogError("Invalid argument (iotHubClientHandle=%p, outputName=%p, eventMessageHandle=%p)", iotHubClientHandle, outputName, eventMessageHandle);
        result = IOTHUB_CLIENT_INVALID_ARG;
    }
    else
    {
        // Codes_SRS_IOTHUBCLIENT_LL_31_128: [ `IoTHubClient_LL_SendEventToOutputAsync` shall set the outputName of the message to send. ]
        if (IoTHubMessage_SetOutputName(eventMessageHandle, outputName) != IOTHUB_MESSAGE_OK)
        {
            LogError("IoTHubMessage_SetOutputName failed");
            result = IOTHUB_CLIENT_ERROR;
        }
        // Codes_SRS_IOTHUBCLIENT_LL_31_129: [ `IoTHubClient_LL_SendEventToOutputAsync` shall invoke `IoTHubClient_LL_SendEventAsync` to send the message. ]
        else if ((result = IoTHubClientCore_LL_SendEventAsync(iotHubClientHandle, eventMessageHandle, eventConfirmationCallback, userContextCallback)) != IOTHUB_CLIENT_OK)
        {
            LogError("Call into IoTHubClient_LL_SendEventAsync failed, result=%d", result);
        }
    }
    return result;
}

static IOTHUB_CLIENT_RESULT create_event_handler_callback(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData, const char* inputName, IOTHUB_CLIENT_MESSAGE_CALLBACK_ASYNC callbackSync, IOTHUB_CLIENT_MESSAGE_CALLBACK_ASYNC_EX callbackSyncEx, void* userContextCallback, void* userContextCallbackEx, size_t userContextCallbackExLength)
{
    IOTHUB_CLIENT_RESULT result = IOTHUB_CLIENT_ERROR;
    bool add_to_list = false;

    if ((handleData->event_callbacks == NULL) && ((handleData->event_callbacks = singlylinkedlist_create()) == NULL))
    {
        LogError("Could not allocate linked list for callbacks");
        result = IOTHUB_CLIENT_ERROR;
    }
    else
    {
        IOTHUB_EVENT_CALLBACK* event_callback = NULL;
        LIST_ITEM_HANDLE item_handle = singlylinkedlist_find(handleData->event_callbacks, is_event_equal_for_match, (const void*)inputName);
        if (item_handle == NULL)
        {
            // Codes_SRS_IOTHUBCLIENT_LL_31_134: [ `IoTHubClient_LL_SetInputMessageCallback` shall allocate a callback handle to associate callbacks from the transport => client if `inputName` isn't already present in the callback list. ]
            event_callback = (IOTHUB_EVENT_CALLBACK*)malloc(sizeof(IOTHUB_EVENT_CALLBACK));
            if (event_callback == NULL)
            {
                LogError("Could not allocate IOTHUB_EVENT_CALLBACK");
                result = IOTHUB_CLIENT_ERROR;
            }
            else
            {
                memset(event_callback, 0, sizeof(*event_callback));
                add_to_list = true;
            }
        }
        else
        {
            // Codes_SRS_IOTHUBCLIENT_LL_31_135: [ `IoTHubClient_LL_SetInputMessageCallback` shall reuse the existing callback handle if `inputName` is already present in the callback list. ]
            event_callback = (IOTHUB_EVENT_CALLBACK*)singlylinkedlist_item_get_value(item_handle);
            if (event_callback == NULL)
            {
                LogError("singlylinkedlist_item_get_value failed looking up event callback");
            }
        }

        if (event_callback != NULL)
        {
            if ((inputName != NULL) && (event_callback->inputName == NULL))
            {
                event_callback->inputName = STRING_construct(inputName);
            }

            if ((inputName == NULL) || (event_callback->inputName != NULL))
            {
                event_callback->callbackAsync = callbackSync;
                event_callback->callbackAsyncEx = callbackSyncEx;

                free(event_callback->userContextCallbackEx);
                event_callback->userContextCallbackEx = NULL;

                if (userContextCallbackEx == NULL)
                {
                    event_callback->userContextCallback = userContextCallback;
                }

                if ((userContextCallbackEx != NULL) &&
                    (NULL == (event_callback->userContextCallbackEx = malloc(userContextCallbackExLength))))
                {
                    LogError("Unable to allocate userContextCallback");
                    delete_event(event_callback);
                    result = IOTHUB_CLIENT_ERROR;
                }
                else if ((add_to_list == true) && (NULL == singlylinkedlist_add(handleData->event_callbacks, event_callback)))
                {
                    LogError("Unable to add eventCallback to list");
                    delete_event(event_callback);
                    result = IOTHUB_CLIENT_ERROR;
                }
                else
                {
                    if (userContextCallbackEx != NULL)
                    {
                        // Codes_SRS_IOTHUBCLIENT_LL_31_141: [`IoTHubClient_LL_SetInputMessageCallbackEx` shall copy the data passed in extended context. ]
                        memcpy(event_callback->userContextCallbackEx, userContextCallbackEx, userContextCallbackExLength);
                    }
                    result = IOTHUB_CLIENT_OK;
                }
            }
            else
            {
                delete_event(event_callback);
                result = IOTHUB_CLIENT_ERROR;
            }
        }
    }

    return result;
}

static IOTHUB_CLIENT_RESULT remove_event_unsubscribe_if_needed(IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData, const char* inputName)
{
    IOTHUB_CLIENT_RESULT result;

    LIST_ITEM_HANDLE item_handle = singlylinkedlist_find(handleData->event_callbacks, is_event_equal_for_match, (const void*)inputName);
    if (item_handle == NULL)
    {
        // Codes_SRS_IOTHUBCLIENT_LL_31_132: [ If `eventHandlerCallback` is NULL, `IoTHubClient_LL_SetInputMessageCallback` shall return `IOTHUB_CLIENT_ERROR` if the `inputName` is not present. ]
        LogError("Input name %s was not present", inputName);
        result = IOTHUB_CLIENT_ERROR;
    }
    else
    {
        IOTHUB_EVENT_CALLBACK* event_callback = (IOTHUB_EVENT_CALLBACK*)singlylinkedlist_item_get_value(item_handle);
        if (event_callback == NULL)
        {
            LogError("singlylinkedlist_item_get_value failed");
            result = IOTHUB_CLIENT_ERROR;
        }
        else
        {
            delete_event(event_callback);
            // Codes_SRS_IOTHUBCLIENT_LL_31_131: [ If `eventHandlerCallback` is NULL, `IoTHubClient_LL_SetInputMessageCallback` shall remove the `inputName` from its callback list if present. ]
            if (singlylinkedlist_remove(handleData->event_callbacks, item_handle) != 0)
            {
                LogError("singlylinkedlist_remove failed");
                result = IOTHUB_CLIENT_ERROR;
            }
            else
            {
                if (singlylinkedlist_get_head_item(handleData->event_callbacks) == NULL)
                {
                    // Codes_SRS_IOTHUBCLIENT_LL_31_133: [ If `eventHandlerCallback` is NULL, `IoTHubClient_LL_SetInputMessageCallback` shall invoke `IoTHubTransport_Unsubscribe_InputQueue` if this was the last input callback. ]
                    handleData->IoTHubTransport_Unsubscribe_InputQueue(handleData);
                }
                result = IOTHUB_CLIENT_OK;
            }
        }
    }

    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SetInputMessageCallbackImpl(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, const char* inputName, IOTHUB_CLIENT_MESSAGE_CALLBACK_ASYNC eventHandlerCallback, IOTHUB_CLIENT_MESSAGE_CALLBACK_ASYNC_EX eventHandlerCallbackEx, void *userContextCallback, void *userContextCallbackEx, size_t userContextCallbackExLength)
{
    IOTHUB_CLIENT_RESULT result;

    if (iotHubClientHandle == NULL)
    {
        // Codes_SRS_IOTHUBCLIENT_LL_31_130: [ If `iotHubClientHandle` or `inputName` is NULL, `IoTHubClient_LL_SetInputMessageCallback` shall return IOTHUB_CLIENT_INVALID_ARG. ]
        LogError("Invalid argument - iotHubClientHandle=%p, inputName=%p", iotHubClientHandle, inputName);
        result = IOTHUB_CLIENT_INVALID_ARG;
    }
    else if (iotHubClientHandle->registration_state != OP_STATE_IOT_STAGE)
    {
        LogError("unable to get send status till device is provisioned");
        result = IOTHUB_CLIENT_PROVISIONING_NOT_COMPLETE;
    }
    else
    {
        IOTHUB_CLIENT_CORE_LL_HANDLE_DATA* handleData = (IOTHUB_CLIENT_CORE_LL_HANDLE_DATA*)iotHubClientHandle;
        if ((eventHandlerCallback == NULL) && (eventHandlerCallbackEx == NULL))
        {
            result = (IOTHUB_CLIENT_RESULT)remove_event_unsubscribe_if_needed(handleData, inputName);
        }
        else
        {
            bool registered_with_transport_handler = (handleData->event_callbacks != NULL) && (singlylinkedlist_get_head_item(handleData->event_callbacks) != NULL);
            if ((result = (IOTHUB_CLIENT_RESULT)create_event_handler_callback(handleData, inputName, eventHandlerCallback, eventHandlerCallbackEx, userContextCallback, userContextCallbackEx, userContextCallbackExLength)) != IOTHUB_CLIENT_OK)
            {
                LogError("create_event_handler_callback call failed, error = %d", result);
            }
            // Codes_SRS_IOTHUBCLIENT_LL_31_136: [ `IoTHubClient_LL_SetInputMessageCallback` shall invoke `IoTHubTransport_Subscribe_InputQueue` if this is the first callback being registered. ]
            else if (!registered_with_transport_handler && (handleData->IoTHubTransport_Subscribe_InputQueue(handleData->deviceHandle) != 0))
            {
                LogError("IoTHubTransport_Subscribe_InputQueue failed");
                delete_event_callback_list(handleData);
                result = IOTHUB_CLIENT_ERROR;
            }
            else
            {
                result = IOTHUB_CLIENT_OK;
            }
        }
    }
    return result;
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SetInputMessageCallbackEx(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, const char* inputName, IOTHUB_CLIENT_MESSAGE_CALLBACK_ASYNC_EX eventHandlerCallbackEx, void *userContextCallbackEx, size_t userContextCallbackExLength)
{
    return IoTHubClientCore_LL_SetInputMessageCallbackImpl(iotHubClientHandle, inputName, NULL, eventHandlerCallbackEx, NULL, userContextCallbackEx, userContextCallbackExLength);
}

IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_SetInputMessageCallback(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, const char* inputName, IOTHUB_CLIENT_MESSAGE_CALLBACK_ASYNC eventHandlerCallback, void* userContextCallback)
{
    return IoTHubClientCore_LL_SetInputMessageCallbackImpl(iotHubClientHandle, inputName, eventHandlerCallback, NULL, userContextCallback, NULL, 0);
}

int IoTHubClientCore_LL_GetTransportCallbacks(TRANSPORT_CALLBACKS_INFO* transport_cb)
{
    int result;
    if (transport_cb == NULL)
    {
        LogError("Invalid parameter transport callback can not be NULL");
        result = MU_FAILURE;
    }
    else
    {
        transport_cb->send_complete_cb = IoTHubClientCore_LL_SendComplete;
        transport_cb->twin_retrieve_prop_complete_cb = IoTHubClientCore_LL_RetrievePropertyComplete;
        transport_cb->twin_rpt_state_complete_cb = IoTHubClientCore_LL_ReportedStateComplete;
        transport_cb->connection_status_cb = IoTHubClientCore_LL_ConnectionStatusCallBack;
        transport_cb->prod_info_cb = IoTHubClientCore_LL_GetProductInfo;
        transport_cb->msg_input_cb = IoTHubClientCore_LL_MessageCallbackFromInput;
        transport_cb->msg_cb = IoTHubClientCore_LL_MessageCallback;
        transport_cb->method_complete_cb = IoTHubClientCore_LL_DeviceMethodComplete;
        result = 0;
    }
    return result;
}

#ifdef USE_EDGE_MODULES
/* These should be replaced during iothub_client refactor */
IOTHUB_CLIENT_RESULT IoTHubClientCore_LL_GenericMethodInvoke(IOTHUB_CLIENT_CORE_LL_HANDLE iotHubClientHandle, const char* deviceId, const char* moduleId, const char* methodName, const char* methodPayload, unsigned int timeout, int* responseStatus, unsigned char** responsePayload, size_t* responsePayloadSize)
{
    IOTHUB_CLIENT_RESULT result;
    if (iotHubClientHandle != NULL)
    {
        if (moduleId != NULL)
        {
            result = IoTHubClient_Edge_ModuleMethodInvoke(iotHubClientHandle->methodHandle, deviceId, moduleId, methodName, methodPayload, timeout, responseStatus, responsePayload, responsePayloadSize);
        }
        else
        {
            result = IoTHubClient_Edge_DeviceMethodInvoke(iotHubClientHandle->methodHandle, deviceId, methodName, methodPayload, timeout, responseStatus, responsePayload, responsePayloadSize);

        }
    }
    else
    {
        result = IOTHUB_CLIENT_INVALID_ARG;
    }
    return result;
}
#endif

/*end*/

