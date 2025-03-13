/**
 * @file tracer.h
 * @author avatarsd (avatarsd2@gmail.com)
 * @brief Tracer protocol core definitions
 * @version 0.1
 * @date 2025-03-07
 *
 * @copyright Copyright (c) 2025
 *
 */

#ifndef H_TRACER
#define H_TRACER

#include <string>
#include <vector>

#include "serdes.h"
#include "tlv.h"

namespace tracer {

/* VALUE */
template <typename T>
class name_t : public serdes_i {
   public:
    const tlv_t::id_t ID = tlv_t::NAME;

   private:
    T* value;
};

/* NAME */
class name_t : public std::string, serdes_i {
   public:
    const tlv_t::id_t ID = tlv_t::NAME;
};

/* DESCRIPTION */
class name_t : public std::string, serdes_i {
   public:
    const tlv_t::id_t ID = tlv_t::DESCRIPTION;
};

/* SUBSCRIBER */

/* LIST */
// list of serdes objects

/* ERROR */
class error_t : serdes_i {
   public:
    enum class id_t { OK, NOT_FOUND, PERMISSION_DENIED, ERROR };
    id_t operator=(id_t err) { return (this->err = err); }

   private:
    id_t err;
};

/* ACL */

/* SETTINGS */

/* TIME */

/* STATUS */
class status_t : public std::pair<id_t, std::string>, public serdes_i {
   public:
    const tlv_t::id_t ID = tlv_t::PATH;
};

/* PATH */
class path_t : public std::vector<name_t>, public serdes_i {
   public:
    const tlv_t::id_t ID = tlv_t::PATH;
};

/* POINT */
class point_i {
   public:
    point_i(name_t name) : name(name){};
    virtual ~point_i() = default;
    virtual status_t read(const path_t& path, tlv_t& data) = 0;
    virtual status_t write(const path_t& path, const tlv_t& data) = 0;
    virtual status_t connect(const path_t& from, const path_t& to) = 0;
    virtual status_t disconnect(const path_t& from, const path_t& to) = 0;

   private:
    name_t name;
};

class point_t : public point_i, public serdes_i {
   public:
    const tlv_t::id_t ID = tlv_t::POINT;
};

/* ROUTER */
class router_t : public point_i, public serdes_i {
   public:
    const tlv_t::id_t ID = tlv_t::ROUTER;
};

};  // namespace tracer

#endif