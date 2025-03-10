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

enum class id_t { OK, NOT_FOUND, PERMISSION_DENIED, ERROR };

/* VALUE */
/* NAME */
using name_t = std::string;
/* DESCRIPTION */
/* SUBSCRIBER */
/* LIST */
// template<typename T>
// using list = std::vector<T>;
/* PATH */
using path_t = std::vector<name_t>;
/* ERROR */
/* STATUS */
using status_t = std::pair<id_t, std::string>;
/* ACL */
/* SETTINGS */
/* TIME */

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

class data_point_t : public point_i, public serdes_i {
   public:
    const tlv_t::id_t ID = tlv_t::PATH;
};

class data_status_t : public point_i, public serdes_i {
   public:
    status_t status;

    const tlv_t::id_t ID = tlv_t::PATH;
};

class data_path_t : public point_i, public serdes_i {
   public:
    path_t path;

    const tlv_t::id_t ID = tlv_t::PATH;
};

};  // namespace tracer

#endif