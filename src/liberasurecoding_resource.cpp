#include <boost/container_hash/hash.hpp>
#include <boost/functional/hash.hpp>

#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <span>
#include <memory>
#include <sstream>

#include "irods/irods_resource_plugin.hpp"
#include "irods/irods_resource_redirect.hpp"
#include "irods/irods_query.hpp"
#include "irods/irods_logger.hpp"
#include "irods/irods_resource_constants.hpp"

#include "libconveyor/conveyor_modern.hpp"
#include "erasurecoding/liberasurecode_wrapper.hpp"
#include "erasurecoding/graph_schema.hpp"

#ifdef HAS_LITE3CLIENT
#include "lite3-cpp/client.hpp"
#endif

namespace {
    using log_resc = irods::experimental::log::resource;

    struct plugin_config {
        int k = 4;
        int m = 2;
        ec_backend_id_t backend = EC_BACKEND_JERASURE_RS_VAND;
        std::unique_ptr<irods::erasurecoding::ErasureCoder> coder;
        std::vector<std::unique_ptr<libconveyor::v2::Conveyor>> pipelines;
#ifdef HAS_LITE3CLIENT
        std::unique_ptr<lite3::Client> l3_client;
#endif
    };

    // --- libconveyor Storage Adapters (Phase 4.2) ---
    // These will be properly implemented in Phase 4.
    ssize_t dummy_pwrite(storage_handle_t, const void*, size_t, off_t) { return 0; }
    ssize_t dummy_pread(storage_handle_t, void*, size_t, off_t) { return 0; }
    off_t dummy_lseek(storage_handle_t, off_t, int) { return 0; }

    void parse_context(const std::string& _context, plugin_config& _cfg) {
        if (_context.empty()) return;
        
        std::stringstream ss(_context);
        std::string token;
        while (std::getline(ss, token, ';')) {
            size_t pos = token.find('=');
            if (pos == std::string::npos) continue;
            
            std::string key = token.substr(0, pos);
            std::string val = token.substr(pos + 1);
            
            if (key == "k") _cfg.k = std::stoi(val);
            else if (key == "m") _cfg.m = std::stoi(val);
            else if (key == "backend") {
                if (val == "jerasure") _cfg.backend = EC_BACKEND_JERASURE_RS_VAND;
                else if (val == "isa_l") _cfg.backend = EC_BACKEND_ISA_L_RS_VAND;
                else if (val == "liberasurecode") _cfg.backend = EC_BACKEND_LIBERASURECODE_RS_VAND;
            }
        }
    }

    irods::error start_operation(irods::plugin_property_map& _prop_map) {
        log_resc::info("erasurecoding: Starting resource operation");
        
        std::string context;
        _prop_map.get<std::string>(irods::RESOURCE_CONTEXT, context);
        
        auto cfg = std::make_shared<plugin_config>();
        parse_context(context, *cfg);
        
        int n = cfg->k + cfg->m;
        log_resc::info("erasurecoding: Initializing backend [{}] with k={}, m={}, total_pipelines={}", 
                      static_cast<int>(cfg->backend), cfg->k, cfg->m, n);
        
        cfg->coder = std::make_unique<irods::erasurecoding::ErasureCoder>(cfg->backend, cfg->k, cfg->m);

#ifdef HAS_LITE3CLIENT
        // Default to localhost for now
        cfg->l3_client = std::make_unique<lite3::Client>("localhost", 5555);
#endif

        // Phase 1.2.1: Instantiate pipelines
        libconveyor::v2::Config conv_cfg;
        conv_cfg.ops = { dummy_pwrite, dummy_pread, dummy_lseek };
        conv_cfg.write_capacity = 32 * 1024 * 1024; // 32MB default chunk boundaries (Phase 1.2.2)
        conv_cfg.read_capacity = 32 * 1024 * 1024;

        for (int i = 0; i < n; ++i) {
            auto res = libconveyor::v2::Conveyor::create(conv_cfg);
            if (!res) {
                return ERROR(SYS_INTERNAL_ERR, "Failed to create libconveyor pipeline " + std::to_string(i));
            }
            cfg->pipelines.push_back(std::make_unique<libconveyor::v2::Conveyor>(std::move(res.value())));
        }
        
        _prop_map.set<std::shared_ptr<plugin_config>>("plugin_config", std::move(cfg));
        
        return SUCCESS();
    }

    irods::error stop_operation(irods::plugin_property_map& _prop_map) {
        log_resc::info("erasurecoding: Stopping resource operation");
        return SUCCESS();
    }

    // --- iRODS Plugin Interface Hooks ---

    irods::error plugin_file_create(irods::plugin_context& _ctx) {
        log_resc::info("erasurecoding: file_create intercept (Phase 4.1.1)");
        return SUCCESS();
    }

    irods::error plugin_file_write(irods::plugin_context& _ctx, void* _buf, int _len) {
        // Use C++20 std::span for zero-copy views
        std::span<std::byte> data(static_cast<std::byte*>(_buf), static_cast<size_t>(_len));
        log_resc::debug("erasurecoding: writing {} bytes using std::span view", data.size());
        
        // Phase 4.1.2: Dispatch fragments to libconveyor
        return SUCCESS();
    }

    irods::error plugin_file_read(irods::plugin_context& _ctx, void* _buf, int _len) {
        log_resc::info("erasurecoding: file_read intercept (Phase 5.1)");
        return SUCCESS();
    }

    irods::error plugin_file_unlink(irods::plugin_context& _ctx) {
        log_resc::info("erasurecoding: file_unlink intercept (Phase 6.1)");
        return SUCCESS();
    }

    // --- Redirect (Voter) Logic ---
    irods::error plugin_redirect(irods::plugin_context& _ctx, const std::string* _op, const std::string* _curr_host, irods::hierarchy_parser* _out_parser, float* _out_vote) {
        // Phase 3.1: The redirect Hijack (Anti-Affinity Voting)
        *_out_vote = 1.0f; 
        return SUCCESS();
    }

} // namespace

extern "C"
irods::resource* plugin_factory(const std::string& _inst_name, const std::string& _context) {
    irods::resource* resc = new irods::resource(_inst_name, _context);

    // Wire up operations
    resc->set_start_operation(start_operation);
    resc->set_stop_operation(stop_operation);
    
    resc->add_operation(irods::RESOURCE_OP_CREATE, std::function<irods::error(irods::plugin_context&)>(plugin_file_create));
    resc->add_operation<void*, int>(irods::RESOURCE_OP_WRITE, std::function<irods::error(irods::plugin_context&, void*, int)>(plugin_file_write));
    resc->add_operation<void*, int>(irods::RESOURCE_OP_READ, std::function<irods::error(irods::plugin_context&, void*, int)>(plugin_file_read));
    resc->add_operation(irods::RESOURCE_OP_UNLINK, std::function<irods::error(irods::plugin_context&)>(plugin_file_unlink));

    resc->add_operation<const std::string*, const std::string*, irods::hierarchy_parser*, float*>(
        irods::RESOURCE_OP_RESOLVE_RESC_HIER, 
        std::function<irods::error(irods::plugin_context&, const std::string*, const std::string*, irods::hierarchy_parser*, float*)>(plugin_redirect));

    return resc;
}
