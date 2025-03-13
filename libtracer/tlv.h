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
        POINT = 0x07,       /**< Endpoint with related data */
        ERROR = 0x08,       /**< Error code */
        STATUS = 0x09,      /**< Containing communication error code and description, empty if ok */
        ACL = 0x0A,         /**< Access control list */
        SETTINGS = 0x0B,    /**< Settings (e.g., bandwidth, ACLs) */
        TIME = 0x0C,        /**< Timestamp (64-bit nanoseconds) */
        ROUTER = 0x0D       /**< Router object */
    };

    /** @brief Options bitfield for TLV header. */
    struct opt_t {
        uint8_t TIMESTAMP : 1; /**< @brief Flag for timestamp inclusion. */
        uint8_t CRC : 1;       /**< @brief Flag for CRC usage. */
        uint8_t _RES : 6;      /**< @brief Reserved bits. */
    };

    /**
     * @brief Header struct for safe memory handling (no flexible array member).
     */
    struct header_t {
        tlv_t::id_t type;  /**< @brief TLV type field. */
        tlv_t::opt_t _res; /**< @brief TLV options field. */
        uint16_t crc;      /**< @brief CRC checksum. */
        uint32_t length;   /**< @brief Length of data payload. */
    };

    header_t header; /**< @brief TLV type field. */
    uint8_t data[];  /**< @brief Flexible array for data (not used directly). */
} /* packed(aligned by sizes) */;

#endif