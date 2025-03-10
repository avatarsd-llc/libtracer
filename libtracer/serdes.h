/**
 * @file serdes.h
 * @author avatarsd (you@domain.com)
 * @brief 
 * @version 0.1
 * @date 2025-03-10
 * 
 * @copyright Copyright (c) 2025
 * 
 */

#ifndef SERDES_I
#define SERDES_I

/* will convert tlv_t to internal type and vice versa */
class serdes_i {
    public:
    virtual void serialize() = 0;
    virtual void deserialize() = 0;
};

#endif
