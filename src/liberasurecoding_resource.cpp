#include "irods/irods_resource_plugin.hpp"
#include "irods/irods_resource_redirect.hpp"
#include "irods/irods_query.hpp"
#include "irods/irods_logger.hpp"

#include "conveyor/conveyor.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <span>
#include <memory>

// liberasurecode placeholder - Phase 1.1.2 will implement the RAII wrapper
// #include <erasurecode.h>

namespace {
    using log_resc = irods::experimental::log::resource;

    // --- Core C++20 Interface Implementations ---

    irods::error start_operation(irods::plugin_property_map& _prop_map) {
        log_resc::info("erasurecoding: Starting resource operation");
        // Phase 1.2.1: Pipeline instantiation will happen here
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
        std::span<std::byte> data(static_cast<std::byte*>(_buf), _len);
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
    resc->add_operation(irods::RESOURCE_OP_START, std::function<irods::error(irods::plugin_property_map&)>(start_operation));
    resc->add_operation(irods::RESOURCE_OP_STOP, std::function<irods::error(irods::plugin_property_map&)>(stop_operation));
    
    resc->add_operation(irods::RESOURCE_OP_FILE_CREATE, std::function<irods::error(irods::plugin_context&)>(plugin_file_create));
    resc->add_operation<void*, int>(irods::RESOURCE_OP_FILE_WRITE, std::function<irods::error(irods::plugin_context&, void*, int)>(plugin_file_write));
    resc->add_operation<void*, int>(irods::RESOURCE_OP_FILE_READ, std::function<irods::error(irods::plugin_context&, void*, int)>(plugin_file_read));
    resc->add_operation(irods::RESOURCE_OP_FILE_UNLINK, std::function<irods::error(irods::plugin_context&)>(plugin_file_unlink));

    resc->add_operation<const std::string*, const std::string*, irods::hierarchy_parser*, float*>(
        irods::RESOURCE_OP_REDIRECT, 
        std::function<irods::error(irods::plugin_context&, const std::string*, const std::string*, irods::hierarchy_parser*, float*)>(plugin_redirect));

    return resc;
}
