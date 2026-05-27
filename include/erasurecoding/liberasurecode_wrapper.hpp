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

    struct DecodedData {
        char* data = nullptr;
        uint64_t data_len = 0;
        int desc = -1;

        DecodedData(int descriptor) : desc(descriptor) {}

        ~DecodedData() {
            if (data) {
                liberasurecode_decode_cleanup(desc, data);
            }
        }

        std::span<const std::byte> span() const {
            return {reinterpret_cast<const std::byte*>(data), static_cast<size_t>(data_len)};
        }

        // Disable copy
        DecodedData(const DecodedData&) = delete;
        DecodedData& operator=(const DecodedData&) = delete;
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

        std::unique_ptr<DecodedData> decode(const std::vector<std::vector<std::byte>>& available_fragments) {
            auto decoded = std::make_unique<DecodedData>(instance_->descriptor());
            
            // Prepare fragment pointers for C API
            std::vector<char*> fragment_ptrs;
            uint64_t fragment_len = 0;
            for (const auto& frag : available_fragments) {
                if (!frag.empty()) {
                    if (fragment_len == 0) fragment_len = frag.size();
                    fragment_ptrs.push_back(const_cast<char*>(reinterpret_cast<const char*>(frag.data())));
                }
            }

            if (fragment_ptrs.empty()) {
                throw LiberasurecodeError("No fragments provided to decode");
            }

            int ret = liberasurecode_decode(instance_->descriptor(),
                                            fragment_ptrs.data(),
                                            static_cast<int>(fragment_ptrs.size()),
                                            fragment_len,
                                            1, // Force metadata checks
                                            &decoded->data,
                                            &decoded->data_len);
            
            if (ret != 0) {
                throw LiberasurecodeError("liberasurecode_decode failed with error: " + std::to_string(ret));
            }

            return decoded;
        }
        
    private:
        std::unique_ptr<LiberasurecodeInstance> instance_;
    };

} // namespace irods::erasurecoding
