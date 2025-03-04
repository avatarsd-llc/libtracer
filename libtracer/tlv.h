/**
 * @file tlv.h
 * @author avatarsd (avatarsd2@gmail.com)
 * @brief Type-Lengh-Value wrapper
 * @version 0.1
 * @date 2025-03-03
 * 
 * @copyright Copyright (c) 2025
 * 
 */

#ifndef TLV_H
#define TLV_H

#include <inttypes.h>

struct tlv_t {
    /** @brief Enum defining TLV types 
     *         The most hardcoded values in the system
     */
    enum id_t : uint8_t {
        VALUE = 0x01,       /**< Actual data value in endpoint */
        NAME = 0x02,        /**< Name for routing */
        DESCRIPTION = 0x03, /**< Optional description */
        SUBSCRIBER = 0x04,  /**< Subscriber with path and settings */
        LIST = 0x05,        /**< List of TLVs (e.g., subscribers, endpoints) */
        PATH = 0x06,        /**< Routing path as a list of names */
        ENDPOINT = 0x07,    /**< Endpoint with related data */
        ERROR = 0x08,       /**< Error code */
        STATUS = 0x09,      /**< Containing communication error code and description, empty if ok */
        ACL = 0x0A,         /**< Access control list */
        SETTINGS = 0x0B,    /**< Settings (e.g., bandwidth, ACLs) */
        TIMESTAMP = 0x0C    /**< Timestamp of last update (64-bit nanoseconds) */
    };

    id_t type;       /**< TLV type identifier */
    uint8_t _res;    /**< Reserved for padding  */
    uint16_t crc;    /**< CRC16, valid if FLAG_CRC is set */
    uint32_t length; /**< Length of the data field in bytes */
    uint8_t data[];  /**< Flexible array member for variable-length data */
} /* packed(aligned by sizes) */;

#endif