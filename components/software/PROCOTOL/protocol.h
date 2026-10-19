/* GB2312: ESP-IDF protocol packing service for the host application. */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTOCOL_FRAME_HEAD          0xFFu
#define PROTOCOL_FRAME_TAIL          0xAAu
#define PROTOCOL_PACKET_LENGTH       9u
#define PROTOCOL_FRAME_SIZE          10u
#define PROTOCOL_LENGTH_OFFSET       1u
#define PROTOCOL_TYPE_OFFSET         2u
#define PROTOCOL_DATA_ID_OFFSET      3u
#define PROTOCOL_DATA_FORMAT_OFFSET  4u
#define PROTOCOL_PAYLOAD_OFFSET      5u
#define PROTOCOL_TAIL_OFFSET         9u

typedef enum {
    PROTOCOL_PACKET_TYPE_ID_READ = 0x01,
    PROTOCOL_PACKET_TYPE_SET_CMD = 0x02,
    PROTOCOL_PACKET_TYPE_DATA = 0x03,
    PROTOCOL_PACKET_TYPE_ACK = 0x04,
    PROTOCOL_PACKET_TYPE_ERROR = 0x05
} Protocol_PacketType_t;

/* All legacy IDs remain defined; this application only emits ECG/PPG1/PPG2/SBP/DBP. */
typedef enum {
    PROTOCOL_DATA_ID_IDREAD = 0x10,
    PROTOCOL_DATA_ID_SYS = 0x20,
    PROTOCOL_DATA_ID_STR = 0x21,
    PROTOCOL_DATA_ID_INIT_STATUS = 0x22,
    PROTOCOL_DATA_ID_ECG = 0x30,
    PROTOCOL_DATA_ID_PPG1 = 0x31,
    PROTOCOL_DATA_ID_PPG2 = 0x32,
    PROTOCOL_DATA_ID_SBP = 0x33,
    PROTOCOL_DATA_ID_DBP = 0x34
} Protocol_DataId_t;

typedef enum {
    PROTOCOL_DATA_FORMAT_RAW32 = 0x00,
    PROTOCOL_DATA_FORMAT_FLOAT32 = 0x01
} Protocol_DataFormat_t;

/* Starts the single protocol task and its bounded input queue. */
esp_err_t Protocol_Start(void);

/* Enqueue one synchronous AIN0..2 sample. It becomes ECG, PPG1 and PPG2 frames. */
esp_err_t Protocol_PublishSample(const int32_t channels[3]);

/* Enqueue one valid prediction as two independent float32 data frames. */
esp_err_t Protocol_PublishBloodPressure(float sbp, float dbp);

#ifdef __cplusplus
}
#endif

#endif
