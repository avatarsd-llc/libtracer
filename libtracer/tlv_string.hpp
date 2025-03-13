#include <cstring>
#include <stdexcept>
#include <string>

#include "tlv_vector.hpp"

/**
 * @brief A class to handle C-strings within a TLV structure, mimicking std::string.
 */
class tlv_string_t : public tlv_vector_t {
   public:
    /**
     * @brief Default constructor: Initializes an empty string.
     */
    tlv_string_t() : tlv_vector_t(tlv_t::NAME, nullptr, 0) {
        set_length(1);  // Space for null terminator
        resize(8 + 1);  // Header (8 bytes) + null terminator
        data()[8] = '\0';
    }

    /**
     * @brief Constructor from const char*: Initializes with a C-string.
     * @param str The C-string to initialize with.
     */
    tlv_string_t(const char* str)
        : tlv_vector_t(tlv_t::NAME, reinterpret_cast<const uint8_t*>(str), std::strlen(str) + 1) {}

    /**
     * @brief Constructor from std::string: Initializes with a std::string.
     * @param str The std::string to initialize with.
     */
    tlv_string_t(const std::string& str)
        : tlv_vector_t(tlv_t::NAME, reinterpret_cast<const uint8_t*>(str.c_str()),
                       str.size() + 1) {}

    /**
     * @brief Assignment operator from const char*.
     * @param str The C-string to assign.
     * @return Reference to this object.
     */
    tlv_string_t& operator=(const char* str) {
        size_t len = std::strlen(str) + 1;  // Include null terminator
        resize(8 + len);                    // Adjust size for header + string
        set_length(static_cast<uint32_t>(len));
        std::memcpy(data() + 8, str, len);
        if (opt().CRC) {
            calculate_and_set_crc();  // Update CRC if enabled
        }
        return *this;
    }

    /**
     * @brief Assignment operator from std::string.
     * @param str The std::string to assign.
     * @return Reference to this object.
     */
    tlv_string_t& operator=(const std::string& str) {
        size_t len = str.size() + 1;  // Include null terminator
        resize(8 + len);
        set_length(static_cast<uint32_t>(len));
        std::memcpy(data() + 8, str.c_str(), len);
        if (opt().CRC) {
            calculate_and_set_crc();
        }
        return *this;
    }

    /**
     * @brief Get the string data as a const char*.
     * @return Pointer to the null-terminated string.
     */
    const char* c_str() const {
        check_size();
        return reinterpret_cast<const char*>(data() + 8);
    }

    /**
     * @brief Get the string data as a std::string.
     * @return The string data excluding the null terminator.
     */
    std::string str() const {
        check_size();
        return std::string(c_str(), length() - 1);
    }

    /**
     * @brief Get the size of the string (excluding null terminator).
     * @return The size of the string.
     */
    size_t size() const {
        check_size();
        return length() > 0 ? length() - 1 : 0;
    }

    /**
     * @brief Check if the string is empty.
     * @return True if the string is empty, false otherwise.
     */
    bool empty() const { return size() == 0; }

    /**
     * @brief Append a C-string to the current string.
     * @param str The C-string to append.
     */
    void append(const char* str) {
        size_t current_len = size();
        size_t append_len = std::strlen(str);
        size_t new_len = current_len + append_len + 1;  // Include null terminator
        resize(8 + new_len);
        set_length(static_cast<uint32_t>(new_len));
        std::memcpy(data() + 8 + current_len, str, append_len + 1);
        if (opt().CRC) {
            calculate_and_set_crc();
        }
    }

    /**
     * @brief Append a std::string to the current string.
     * @param str The std::string to append.
     */
    void append(const std::string& str) { append(str.c_str()); }

   private:
    /**
     * @brief Set the length of the data, ensuring space for the null terminator.
     * @param len The new length (including null terminator).
     */
    void set_length(uint32_t len) {
        length() = len;
        if (size() < 8 + len) {
            resize(8 + len);
        }
    }
};