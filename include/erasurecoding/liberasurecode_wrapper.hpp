#pragma once

#include <erasurecode.h>
#include <stdexcept>
#include <vector>
#include <string>
#include <span>
#include <memory>

namespace irods::erasurecoding {

    class LiberasurecodeError : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    class LiberasurecodeInstance {
    public:
        LiberasurecodeInstance(ec_backend_id_t backend_id, ec_args& args) {
            desc_ = liberasurecode_instance_create(backend_id, &args);
            if (desc_ <= 0) {
                throw LiberasurecodeError("Failed to create liberasurecode instance");
            }
            k_ = args.k;
            m_ = args.m;
        }

        ~LiberasurecodeInstance() {
            if (desc_ > 0) {
                liberasurecode_instance_destroy(desc_);
            }
        }

        // Disable copy
        LiberasurecodeInstance(const LiberasurecodeInstance&) = delete;
        LiberasurecodeInstance& operator=(const LiberasurecodeInstance&) = delete;

        int descriptor() const { return desc_; }
        int k() const { return k_; }
        int m() const { return m_; }

    private:
        int desc_ = -1;
        int k_ = 0;
        int m_ = 0;
    };

    struct EncodedFragments {
        char** data = nullptr;
        char** parity = nullptr;
        uint64_t fragment_len = 0;
        int desc = -1;
        int k = 0;
        int m = 0;

        EncodedFragments(int descriptor, int _k, int _m) : desc(descriptor), k(_k), m(_m) {}

        ~EncodedFragments() {
            if (data || parity) {
                liberasurecode_encode_cleanup(desc, data, parity);
            }
        }

        // Disable copy
        EncodedFragments(const EncodedFragments&) = delete;
        EncodedFragments& operator=(const EncodedFragments&) = delete;
    };

    class ErasureCoder {
    public:
        ErasureCoder(ec_backend_id_t backend_id, int k, int m) {
            ec_args args{};
            args.k = k;
            args.m = m;
            args.ct = CHKSUM_NONE; // Default for now
            
            instance_ = std::make_unique<LiberasurecodeInstance>(backend_id, args);
        }

        std::unique_ptr<EncodedFragments> encode(std::span<const std::byte> data) {
            auto fragments = std::make_unique<EncodedFragments>(instance_->descriptor(), instance_->k(), instance_->m());
            
            int ret = liberasurecode_encode(instance_->descriptor(),
                                            reinterpret_cast<const char*>(data.data()),
                                            data.size(),
                                            &fragments->data,
                                            &fragments->parity,
                                            &fragments->fragment_len);
            if (ret != 0) {
                throw LiberasurecodeError("liberasurecode_encode failed with error: " + std::to_string(ret));
            }
            return fragments;
        }

        // Decode will be implemented in Phase 5
        
    private:
        std::unique_ptr<LiberasurecodeInstance> instance_;
    };

} // namespace irods::erasurecoding
