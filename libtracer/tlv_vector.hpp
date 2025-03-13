#include <inttypes.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "tlv.h"

/**
 * @brief A vector-based class for managing TLV (Type-Length-Value) structures.
 */
class tlv_vector_t : public std::vector<uint8_t> {
   public:
    /**
     * @brief Default constructor: Initializes with an 8-byte header.
     */
    tlv_vector_t() {
        resize(8);
        tlv_t::header_t header = {tlv_t::VALUE, {0, 0, 0}, 0, 0};
        std::memcpy(data(), &header, 8);
    }

    /**
     * @brief Template constructor: Initializes with type, data, and options.
     * @tparam T Type of the data to store.
     * @param type TLV type identifier.
     * @param data Reference to the data to store.
     * @param opt Optional options (default: no flags set).
     */
    template <typename T>
    tlv_vector_t(tlv_t::id_t type, const T& data, tlv_t::opt_t opt = {0, 0, 0}) {
        resize(8 + sizeof(T));
        tlv_t::header_t header = {type, opt, 0, sizeof(T)};
        std::memcpy(this->data(), &header, 8);
        std::memcpy(this->data() + 8, &data, sizeof(T));
        if (opt.CRC) {
            calculate_and_set_crc();
        }
    }

    /**
     * @brief Raw byte constructor: Initializes with type, raw data, and options.
     * @param type TLV type identifier.
     * @param data Pointer to the raw data.
     * @param length Length of the raw data.
     * @param opt Optional options (default: no flags set).
     */
    tlv_vector_t(tlv_t::id_t type, const uint8_t* data, size_t length,
                 tlv_t::opt_t opt = {0, 0, 0}) {
        resize(8 + length);
        tlv_t::header_t header = {type, opt, 0, static_cast<uint32_t>(length)};
        std::memcpy(this->data(), &header, 8);
        std::memcpy(this->data() + 8, data, length);
        if (opt.CRC) {
            calculate_and_set_crc();
        }
    }

    // Header field accessors (returning references)

    /**
     * @brief Access the TLV type field.
     * @return Reference to the type field.
     */
    tlv_t::id_t& type() {
        check_size();
        return reinterpret_cast<tlv_t::header_t*>(data())->type;
    }
    /** @brief Const access to the TLV type field. */
    const tlv_t::id_t& type() const {
        check_size();
        return reinterpret_cast<const tlv_t::header_t*>(data())->type;
    }

    /**
     * @brief Access the TLV options field.
     * @return Reference to the options bitfield.
     */
    tlv_t::opt_t& opt() {
        check_size();
        return reinterpret_cast<tlv_t::header_t*>(data())->_res;
    }
    /** @brief Const access to the TLV options field. */
    const tlv_t::opt_t& opt() const {
        check_size();
        return reinterpret_cast<const tlv_t::header_t*>(data())->_res;
    }

    /**
     * @brief Access the CRC field.
     * @return Reference to the CRC field.
     */
    uint16_t& crc() {
        check_size();
        return reinterpret_cast<tlv_t::header_t*>(data())->crc;
    }
    /** @brief Const access to the CRC field. */
    const uint16_t& crc() const {
        check_size();
        return reinterpret_cast<const tlv_t::header_t*>(data())->crc;
    }

    /**
     * @brief Access the length field.
     * @return Reference to the length field.
     */
    uint32_t& length() {
        check_size();
        return reinterpret_cast<tlv_t::header_t*>(data())->length;
    }
    /** @brief Const access to the length field. */
    const uint32_t& length() const {
        check_size();
        return reinterpret_cast<const tlv_t::header_t*>(data())->length;
    }

    // Data accessors

    /**
     * @brief Access the data payload as a reference to type T.
     * @tparam T Type to cast the data to.
     * @return Reference to the data.
     * @throws std::runtime_error if insufficient data.
     */
    template <typename T>
    T& value() {
        check_size();
        if (size() < 8 + sizeof(T) || length() < sizeof(T)) {
            throw std::runtime_error("Insufficient data for type T");
        }
        return *reinterpret_cast<T*>(data() + 8);
    }

    /**
     * @brief Const access to the data payload as type T.
     * @tparam T Type to cast the data to.
     * @return Const reference to the data.
     * @throws std::runtime_error if insufficient data.
     */
    template <typename T>
    const T& value() const {
        check_size();
        if (size() < 8 + sizeof(T) || length() < sizeof(T)) {
            throw std::runtime_error("Insufficient data for type T");
        }
        return *reinterpret_cast<const T*>(data() + 8);
    }

    // CRC management

    /**
     * @brief Calculate and set the CRC, enabling the CRC flag.
     */
    void calculate_and_set_crc() {
        check_size();
        crc() = calculate_crc();
        opt().CRC = 1;
    }

    /**
     * @brief Verify the stored CRC against the calculated value.
     * @return True if CRC matches or no CRC is set, false otherwise.
     */
    bool verify_crc() const {
        check_size();
        if (!opt().CRC) {
            return true;  // No CRC to verify
        }
        return calculate_crc() == crc();
    }

   private:
    /**
     * @brief Ensure the buffer is large enough for the header.
     * @throws std::runtime_error if buffer is too small.
     */
    void check_size() const {
        if (size() < 8) {
            throw std::runtime_error("Buffer too small for TLV header");
        }
    }

    /**
     * @brief Calculate a 16-bit XOR CRC of the data payload.
     * @return The calculated CRC value.
     */
    uint16_t calculate_crc() const {
        uint32_t len = length();
        const uint8_t* data_ptr = data() + 8;
        uint16_t crc_val = 0;
        while (len >= 2) {
            uint16_t word;
            std::memcpy(&word, data_ptr, 2);
            crc_val ^= word;
            data_ptr += 2;
            len -= 2;
        }
        if (len == 1) {
            crc_val ^= static_cast<uint16_t>(*data_ptr) << 8;
        }
        return crc_val;
    }
};