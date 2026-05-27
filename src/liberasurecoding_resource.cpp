#include <boost/container_hash/hash.hpp>
#include <boost/functional/hash.hpp>
#include <boost/smart_ptr/make_shared.hpp>

#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <span>
#include <memory>
#include <sstream>
#include <mutex>
#include <unordered_map>

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

#include "irods/irods_erasure_coding_error_codes.hpp"

namespace {
    using log_resc = irods::experimental::log::resource;
    namespace ec_err = irods::erasurecoding::error_codes;

    // --- libconveyor Storage Adapters (Phase 4.2) ---

    struct ec_handle {
        irods::resource_ptr child;
        irods::file_object_ptr fco;
    };

    ssize_t ec_pwrite(storage_handle_t _handle, const void* _buf, size_t _len, off_t _offset) {
        auto* h = static_cast<ec_handle*>(_handle);
        if (!h || !h->child || !h->fco) return ec_err::WRITE_ERROR;
        if (auto ret = h->child->call_without_policy<off_t, int>(nullptr, irods::RESOURCE_OP_LSEEK, h->fco, _offset, SEEK_SET); !ret.ok()) return ec_err::WRITE_ERROR;
        if (auto ret = h->child->call_without_policy<void*, int>(nullptr, irods::RESOURCE_OP_WRITE, h->fco, const_cast<void*>(_buf), static_cast<int>(_len)); !ret.ok()) return ec_err::WRITE_ERROR;
        return static_cast<ssize_t>(_len);
    }

    ssize_t ec_pread(storage_handle_t _handle, void* _buf, size_t _len, off_t _offset) {
        auto* h = static_cast<ec_handle*>(_handle);
        if (!h || !h->child || !h->fco) return ec_err::READ_ERROR;
        if (auto ret = h->child->call_without_policy<off_t, int>(nullptr, irods::RESOURCE_OP_LSEEK, h->fco, _offset, SEEK_SET); !ret.ok()) return ec_err::READ_ERROR;
        if (auto ret = h->child->call_without_policy<void*, int>(nullptr, irods::RESOURCE_OP_READ, h->fco, _buf, static_cast<int>(_len)); !ret.ok()) return ec_err::READ_ERROR;
        return static_cast<ssize_t>(_len);
    }

    off_t ec_lseek(storage_handle_t _handle, off_t _offset, int _whence) {
        auto* h = static_cast<ec_handle*>(_handle);
        if (!h || !h->child || !h->fco) return ec_err::LSEEK_ERROR;
        if (auto ret = h->child->call_without_policy<off_t, int>(nullptr, irods::RESOURCE_OP_LSEEK, h->fco, _offset, _whence); !ret.ok()) return ec_err::LSEEK_ERROR;
        return _offset; 
    }

    struct file_state {
        std::vector<std::unique_ptr<libconveyor::v2::Conveyor>> pipelines;
        std::vector<std::byte> write_accumulator;
        size_t current_offset = 0;
        std::vector<std::shared_ptr<ec_handle>> handles;
        
        file_state(size_t _chunk_size) : current_offset(0) {
            write_accumulator.resize(_chunk_size);
        }
    };

    std::unordered_map<int, std::shared_ptr<file_state>> g_file_states;
    std::mutex g_state_mutex;

    struct plugin_config {
        int k = 4;
        int m = 2;
        size_t chunk_size = 32 * 1024 * 1024;
        ec_backend_id_t backend = EC_BACKEND_JERASURE_RS_VAND;
        std::unique_ptr<irods::erasurecoding::ErasureCoder> coder;
#ifdef HAS_LITE3CLIENT
        std::unique_ptr<lite3::Client> l3_client;
#endif
    };

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
        cfg->coder = std::make_unique<irods::erasurecoding::ErasureCoder>(cfg->backend, cfg->k, cfg->m);
#ifdef HAS_LITE3CLIENT
        cfg->l3_client = std::make_unique<lite3::Client>("localhost", 5555);
#endif
        _prop_map.set<std::shared_ptr<plugin_config>>("plugin_config", std::move(cfg));
        return SUCCESS();
    }

    irods::error stop_operation(irods::plugin_property_map& _prop_map) {
        log_resc::info("erasurecoding: Stopping resource operation");
        return SUCCESS();
    }

    irods::error plugin_file_create(irods::plugin_context& _ctx) {
        irods::file_object_ptr file_obj = boost::dynamic_pointer_cast<irods::file_object>(_ctx.fco());
        if (!file_obj) return ERROR(SYS_INVALID_INPUT_PARAM, "Failed to cast FCO");
        std::shared_ptr<plugin_config> cfg;
        _ctx.prop_map().get<std::shared_ptr<plugin_config>>("plugin_config", cfg);
        auto state = std::make_shared<file_state>(cfg->chunk_size);
        int n = cfg->k + cfg->m;
        irods::resource_child_map* children = nullptr;
        _ctx.prop_map().get<irods::resource_child_map*>(irods::RESC_CHILD_MAP_PROP, children);
        if (!children || children->empty()) return ERROR(SYS_CONFIG_FILE_ERR, "No children for EC");
        int i = 0;
        for (auto& [full_name, child_pair] : *children) {
            if (i >= n) break;
            auto& [context, child_ptr] = child_pair;
            auto handle = std::make_shared<ec_handle>();
            handle->child = child_ptr;
            std::string frag_path = file_obj->physical_path() + ".frag" + std::to_string(i);
            handle->fco = boost::make_shared<irods::file_object>(_ctx.comm(), file_obj->logical_path(), frag_path, "", 0, file_obj->mode(), file_obj->flags());
            if (auto ret = child_ptr->call(_ctx.comm(), irods::RESOURCE_OP_CREATE, handle->fco); !ret.ok()) return ret;
            state->handles.push_back(handle);
            libconveyor::v2::Config conv_cfg;
            conv_cfg.handle = handle.get();
            conv_cfg.ops.pwrite = ec_pwrite;
            conv_cfg.ops.pread = ec_pread;
            conv_cfg.ops.lseek = ec_lseek;
            conv_cfg.write_capacity = (cfg->chunk_size / cfg->k) + 80; // Account for EC header
            conv_cfg.read_capacity = conv_cfg.write_capacity;
            auto res = libconveyor::v2::Conveyor::create(conv_cfg);
            if (!res) return ERROR(SYS_INTERNAL_ERR, "Failed to create pipeline");
            state->pipelines.push_back(std::make_unique<libconveyor::v2::Conveyor>(std::move(res.value())));
            i++;
        }
        if (state->handles.size() < static_cast<size_t>(n)) return ERROR(SYS_CONFIG_FILE_ERR, "Insufficient children");
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_file_states[file_obj->l1_desc_idx()] = state;
        }
#ifdef HAS_LITE3CLIENT
        if (cfg->l3_client) {
            irods::erasurecoding::schema::LogicalFile meta;
            meta.path = file_obj->logical_path();
            meta.size = 0; meta.k = cfg->k; meta.m = cfg->m; meta.backend = "jerasure"; 
            try {
                auto doc = meta.to_json();
                cfg->l3_client->put(meta.path, std::string_view(reinterpret_cast<const char*>(doc.buffer().data()), doc.buffer().size()));

                // Phase 4.1.4: Register Fragments
                for (size_t frag_idx = 0; frag_idx < state->handles.size(); ++frag_idx) {
                    auto& h = state->handles[frag_idx];
                    irods::erasurecoding::schema::Fragment frag;
                    frag.index = static_cast<int>(frag_idx);
                    frag.physical_path = h->fco->physical_path();
                    frag.is_parity = (frag_idx >= static_cast<size_t>(cfg->k));
                    
                    auto frag_doc = frag.to_json();
                    std::string frag_key = meta.path + ".frag" + std::to_string(frag_idx);
                    cfg->l3_client->put(frag_key, std::string_view(reinterpret_cast<const char*>(frag_doc.buffer().data()), frag_doc.buffer().size()));
                }
            } catch (...) {}
        }
#endif
        return SUCCESS();
    }

    irods::error plugin_file_write(irods::plugin_context& _ctx, void* _buf, int _len) {
        std::span<std::byte> data(static_cast<std::byte*>(_buf), static_cast<size_t>(_len));
        
        irods::file_object_ptr file_obj = boost::dynamic_pointer_cast<irods::file_object>(_ctx.fco());
        std::shared_ptr<plugin_config> cfg;
        _ctx.prop_map().get<std::shared_ptr<plugin_config>>("plugin_config", cfg);

        std::shared_ptr<file_state> state;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            auto it = g_file_states.find(file_obj->l1_desc_idx());
            if (it == g_file_states.end()) return ERROR(ec_err::FILE_STATE_NOT_FOUND, "File state not found");
            state = it->second;
        }

        size_t bytes_remaining = data.size();
        size_t data_offset = 0;

        while (bytes_remaining > 0) {
            size_t space_in_buffer = cfg->chunk_size - state->current_offset;
            size_t to_copy = std::min(bytes_remaining, space_in_buffer);
            
            std::memcpy(state->write_accumulator.data() + state->current_offset, data.data() + data_offset, to_copy);
            
            state->current_offset += to_copy;
            data_offset += to_copy;
            bytes_remaining -= to_copy;

            if (state->current_offset >= cfg->chunk_size) {
                try {
                    auto fragments = cfg->coder->encode(state->write_accumulator);
                    for (int i = 0; i < cfg->k; ++i) {
                        state->pipelines[i]->write(std::span(reinterpret_cast<const std::byte*>(fragments->data[i]), fragments->fragment_len));
                    }
                    for (int i = 0; i < cfg->m; ++i) {
                        state->pipelines[cfg->k + i]->write(std::span(reinterpret_cast<const std::byte*>(fragments->parity[i]), fragments->fragment_len));
                    }
                    state->current_offset = 0;
                } catch (const std::exception& e) {
                    return ERROR(ec_err::WRITE_ERROR, e.what());
                }
            }
        }

        return SUCCESS();
    }

    irods::error plugin_file_open(irods::plugin_context& _ctx) {
        log_resc::info("erasurecoding: file_open intercept (Phase 5.1)");

        irods::file_object_ptr file_obj = boost::dynamic_pointer_cast<irods::file_object>(_ctx.fco());
        if (!file_obj) return ERROR(SYS_INVALID_INPUT_PARAM, "Failed to cast FCO");

        std::shared_ptr<plugin_config> cfg;
        _ctx.prop_map().get<std::shared_ptr<plugin_config>>("plugin_config", cfg);

        // Phase 5.2: Query L3KVG
#ifdef HAS_LITE3CLIENT
        if (cfg->l3_client) {
            auto res = cfg->l3_client->get(file_obj->logical_path());
            if (res) {
                log_resc::debug("erasurecoding: Found shadow entry for [{}]", file_obj->logical_path());
                // TODO: Parse metadata to confirm k/m and fragment locations
            }
        }
#endif

        // Resolve children and initialize pipelines (Similar to file_create)
        // For simplicity in this phase, we reuse the same logic
        return plugin_file_create(_ctx);
    }

    irods::error plugin_file_read(irods::plugin_context& _ctx, void* _buf, int _len) {
        log_resc::debug("erasurecoding: file_read request for {} bytes", _len);

        irods::file_object_ptr file_obj = boost::dynamic_pointer_cast<irods::file_object>(_ctx.fco());
        std::shared_ptr<plugin_config> cfg;
        _ctx.prop_map().get<std::shared_ptr<plugin_config>>("plugin_config", cfg);

        std::shared_ptr<file_state> state;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            auto it = g_file_states.find(file_obj->l1_desc_idx());
            if (it == g_file_states.end()) return ERROR(ec_err::FILE_STATE_NOT_FOUND, "File state not found");
            state = it->second;
        }

        // Phase 5.3: Parallel Read and Reassembly
        constexpr size_t header_size = 80; // sizeof(fragment_header_t)
        size_t frag_len = (cfg->chunk_size / cfg->k) + header_size;
        
        std::vector<std::vector<std::byte>> all_fragments(cfg->k + cfg->m);
        int valid_fragment_count = 0;
        std::vector<int> missing_data_indices;

        // 1. Attempt to read 'k' data fragments
        for (int i = 0; i < cfg->k; ++i) {
            all_fragments[i].resize(frag_len);
            std::span<std::byte> frag_span(all_fragments[i]);
            auto res = state->pipelines[i]->read(frag_span);
            if (!res) {
                log_resc::error("erasurecoding: Read failed on data fragment {}", i);
                all_fragments[i].clear();
                missing_data_indices.push_back(i);
            } else {
                valid_fragment_count++;
            }
        }

        // 2. If data is missing, fetch parity fragments until we have 'k' total
        if (!missing_data_indices.empty()) {
            log_resc::info("erasurecoding: Entering recovery, missing {} data fragments", missing_data_indices.size());
            
            for (int i = 0; i < cfg->m && valid_fragment_count < cfg->k; ++i) {
                int parity_idx = cfg->k + i;
                all_fragments[parity_idx].resize(frag_len);
                std::span<std::byte> frag_span(all_fragments[parity_idx]);
                auto res = state->pipelines[parity_idx]->read(frag_span);
                if (res) {
                    valid_fragment_count++;
                } else {
                    all_fragments[parity_idx].clear();
                    log_resc::error("erasurecoding: Read failed on parity fragment {}", i);
                }
            }
        }

        // 3. Reassemble or Decode
        std::span<const std::byte> final_data;
        std::unique_ptr<irods::erasurecoding::DecodedData> decoded_holder;

        if (missing_data_indices.empty()) {
            // Fast path: No reconstruction needed, just reassemble data fragments
            // (Reassembly is done below directly into _buf for performance)
        } else if (valid_fragment_count >= cfg->k) {
            // Recovery path: Reconstruction needed
            try {
                decoded_holder = cfg->coder->decode(all_fragments);
                final_data = decoded_holder->span();
            } catch (const std::exception& e) {
                return ERROR(ec_err::READ_ERROR, "Reconstruction failed: " + std::string(e.what()));
            }
        } else {
            return ERROR(ec_err::READ_ERROR, "Insufficient fragments for recovery (" + std::to_string(valid_fragment_count) + "/" + std::to_string(cfg->k) + ")");
        }

        // 4. Reassemble data into output buffer
        char* out_ptr = static_cast<char*>(_buf);

        if (decoded_holder) {
            // Use reconstructed data (already stripped of headers by decode)
            size_t to_copy = std::min(final_data.size(), static_cast<size_t>(_len));
            std::memcpy(out_ptr, final_data.data(), to_copy);
        } else {
            // Reassemble from valid data fragments (must skip headers!)
            size_t total_read = 0;
            for (int i = 0; i < cfg->k && total_read < static_cast<size_t>(_len); ++i) {
                if (all_fragments[i].size() <= header_size) continue;
                
                size_t actual_data_in_frag = all_fragments[i].size() - header_size;
                size_t to_copy = std::min(actual_data_in_frag, static_cast<size_t>(_len) - total_read);
                
                std::memcpy(out_ptr + total_read, all_fragments[i].data() + header_size, to_copy);
                total_read += to_copy;
            }
        }

        return SUCCESS();
    }

    irods::error plugin_file_close(irods::plugin_context& _ctx) {
        irods::file_object_ptr file_obj = boost::dynamic_pointer_cast<irods::file_object>(_ctx.fco());
        std::shared_ptr<plugin_config> cfg;
        _ctx.prop_map().get<std::shared_ptr<plugin_config>>("plugin_config", cfg);

        std::shared_ptr<file_state> state;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            auto it = g_file_states.find(file_obj->l1_desc_idx());
            if (it == g_file_states.end()) return SUCCESS();
            state = it->second;
            g_file_states.erase(it);
        }

        // Phase 4.1.5: Final Padding and Flush
        if (state->current_offset > 0) {
            log_resc::info("erasurecoding: Padding final chunk ({} bytes)", state->current_offset);
            // Pad the rest of the pre-allocated buffer with zeros
            std::memset(state->write_accumulator.data() + state->current_offset, 0, cfg->chunk_size - state->current_offset);
            
            try {
                auto fragments = cfg->coder->encode(state->write_accumulator);
                for (int i = 0; i < cfg->k; ++i) {
                    state->pipelines[i]->write(std::span(reinterpret_cast<const std::byte*>(fragments->data[i]), fragments->fragment_len));
                }
                for (int i = 0; i < cfg->m; ++i) {
                    state->pipelines[cfg->k + i]->write(std::span(reinterpret_cast<const std::byte*>(fragments->parity[i]), fragments->fragment_len));
                }
            } catch (const std::exception& e) {
                log_resc::error("erasurecoding: Final encode failed: {}", e.what());
            }
        }

        for (auto& p : state->pipelines) p->flush();
        for (auto& h : state->handles) h->child->call(_ctx.comm(), irods::RESOURCE_OP_CLOSE, h->fco);
        
        return SUCCESS();
    }

    irods::error plugin_file_unlink(irods::plugin_context& _ctx) {
        log_resc::info("erasurecoding: file_unlink intercept (Phase 6.1)");

        irods::file_object_ptr file_obj = boost::dynamic_pointer_cast<irods::file_object>(_ctx.fco());
        if (!file_obj) return ERROR(SYS_INVALID_INPUT_PARAM, "Failed to cast FCO");

        std::shared_ptr<plugin_config> cfg;
        _ctx.prop_map().get<std::shared_ptr<plugin_config>>("plugin_config", cfg);

        irods::resource_child_map* children = nullptr;
        _ctx.prop_map().get<irods::resource_child_map*>(irods::RESC_CHILD_MAP_PROP, children);

        int n = cfg->k + cfg->m;
        int i = 0;
        for (auto& [full_name, child_pair] : *children) {
            if (i >= n) break;
            
            auto& [context, child_ptr] = child_pair;
            
            // Reconstruct fragment FCO
            std::string frag_path = file_obj->physical_path() + ".frag" + std::to_string(i);
            auto frag_fco = boost::make_shared<irods::file_object>(
                _ctx.comm(), file_obj->logical_path(), frag_path, "", 0, 0, 0);

            // Phase 6.2: Scrub physical fragment
            child_ptr->call(_ctx.comm(), irods::RESOURCE_OP_UNLINK, frag_fco);
            
            // Phase 6.3: Cleanup L3KVG
#ifdef HAS_LITE3CLIENT
            if (cfg->l3_client) {
                std::string frag_key = file_obj->logical_path() + ".frag" + std::to_string(i);
                cfg->l3_client->del(frag_key);
            }
#endif
            i++;
        }

#ifdef HAS_LITE3CLIENT
        if (cfg->l3_client) {
            cfg->l3_client->del(file_obj->logical_path());
        }
#endif

        return SUCCESS();
    }

    // --- Redirect (Voter) Logic ---
    irods::error plugin_redirect(irods::plugin_context& _ctx, const std::string* _op, const std::string* _curr_host, irods::hierarchy_parser* _out_parser, float* _out_vote) {
        if (!_op || !_out_parser || !_out_vote) {
            return ERROR(SYS_INVALID_INPUT_PARAM, "Null input parameter in redirect");
        }

        log_resc::debug("erasurecoding: Redirect request for operation [{}]", *_op);

        std::shared_ptr<plugin_config> cfg;
        _ctx.prop_map().get<std::shared_ptr<plugin_config>>("plugin_config", cfg);
        
        irods::resource_child_map* children = nullptr;
        _ctx.prop_map().get<irods::resource_child_map*>(irods::RESC_CHILD_MAP_PROP, children);
        
        if (!children || children->empty()) {
            log_resc::error("erasurecoding: No children configured");
            *_out_vote = 0.0f;
            return SUCCESS();
        }

        // Phase 3.2: Optimal Anti-Affinity Analysis
        // Group children by host to identify physical dispersion
        std::unordered_map<std::string, std::vector<std::string>> host_map;
        for (auto& [full_name, child_pair] : *children) {
            auto& [context, child_ptr] = child_pair;
            std::string location;
            child_ptr->get_property<std::string>(irods::RESOURCE_LOCATION, location);
            host_map[location].push_back(full_name);
        }

        int n_required = cfg->k + cfg->m;
        int distinct_hosts = static_cast<int>(host_map.size());

        if (*_op == irods::CREATE_OPERATION || *_op == irods::WRITE_OPERATION) {
            if (distinct_hosts < n_required) {
                log_resc::warn("erasurecoding: Insufficient physical dispersion ({} hosts, need {}). Down-voting.", 
                              distinct_hosts, n_required);
                *_out_vote = 0.1f; // Prefer other resources if available
            } else {
                log_resc::info("erasurecoding: Optimal dispersion verified ({} distinct hosts).", distinct_hosts);
                *_out_vote = 1.0f;
            }
        } else {
            // For reads and other ops, we must stay in the path to handle reassembly
            *_out_vote = 1.0f;
        }

        // Add ourselves to the hierarchy
        std::string resc_name;
        _ctx.prop_map().get<std::string>(irods::RESOURCE_NAME, resc_name);
        _out_parser->add_child(resc_name);

        return SUCCESS();
    }

} // namespace

extern "C"
irods::resource* plugin_factory(const std::string& _inst_name, const std::string& _context) {
    irods::resource* resc = new irods::resource(_inst_name, _context);
    resc->set_start_operation(start_operation);
    resc->set_stop_operation(stop_operation);
    resc->add_operation(irods::RESOURCE_OP_CREATE, std::function<irods::error(irods::plugin_context&)>(plugin_file_create));
    resc->add_operation(irods::RESOURCE_OP_OPEN, std::function<irods::error(irods::plugin_context&)>(plugin_file_open));
    resc->add_operation<void*, int>(irods::RESOURCE_OP_WRITE, std::function<irods::error(irods::plugin_context&, void*, int)>(plugin_file_write));
    resc->add_operation<void*, int>(irods::RESOURCE_OP_READ, std::function<irods::error(irods::plugin_context&, void*, int)>(plugin_file_read));
    resc->add_operation(irods::RESOURCE_OP_CLOSE, std::function<irods::error(irods::plugin_context&)>(plugin_file_close));
    resc->add_operation(irods::RESOURCE_OP_UNLINK, std::function<irods::error(irods::plugin_context&)>(plugin_file_unlink));
    resc->add_operation<const std::string*, const std::string*, irods::hierarchy_parser*, float*>(irods::RESOURCE_OP_RESOLVE_RESC_HIER, std::function<irods::error(irods::plugin_context&, const std::string*, const std::string*, irods::hierarchy_parser*, float*)>(plugin_redirect));
    return resc;
}
