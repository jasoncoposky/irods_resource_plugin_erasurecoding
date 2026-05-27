#pragma once

#include "document.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace irods::erasurecoding::schema {

    /**
     * @brief Schema for LOGICAL_FILE nodes in L3KVG.
     */
    struct LogicalFile {
        std::string path;
        uint64_t size;
        uint64_t create_ts;
        uint64_t modify_ts;
        std::string checksum;
        int k;
        int m;
        std::string backend;

        lite3cpp::Document to_json() const {
            lite3cpp::Document doc;
            auto obj = doc.root_obj();
            obj["type"] = "LOGICAL_FILE";
            obj["path"] = path;
            obj["size"] = static_cast<int64_t>(size);
            obj["create_ts"] = static_cast<int64_t>(create_ts);
            obj["modify_ts"] = static_cast<int64_t>(modify_ts);
            obj["checksum"] = checksum;
            obj["k"] = static_cast<int64_t>(k);
            obj["m"] = static_cast<int64_t>(m);
            obj["backend"] = backend;
            return doc;
        }
    };

    /**
     * @brief Schema for FRAGMENT nodes.
     */
    struct Fragment {
        int index;
        uint64_t size;
        std::string physical_path;
        std::string resource_name;
        bool is_parity;

        lite3cpp::Document to_json() const {
            lite3cpp::Document doc;
            auto obj = doc.root_obj();
            obj["type"] = "FRAGMENT";
            obj["index"] = static_cast<int64_t>(index);
            obj["size"] = static_cast<int64_t>(size);
            obj["physical_path"] = physical_path;
            obj["resource_name"] = resource_name;
            obj["is_parity"] = is_parity;
            return doc;
        }
    };

} // namespace irods::erasurecoding::schema
