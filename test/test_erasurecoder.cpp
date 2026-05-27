#include <gtest/gtest.h>
#include "erasurecoding/liberasurecode_wrapper.hpp"
#include <vector>
#include <random>
#include <algorithm>

using namespace irods::erasurecoding;

class ErasureCoderTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Try multiple backends in order of preference
        std::vector<ec_backend_id_t> backends = {
            EC_BACKEND_JERASURE_RS_VAND,
            EC_BACKEND_LIBERASURECODE_RS_VAND,
            EC_BACKEND_FLAT_XOR_HD
        };

        for (auto b : backends) {
            try {
                coder_ = std::make_unique<ErasureCoder>(b, k, m);
                selected_backend = b;
                break;
            } catch (...) {
                continue;
            }
        }

        if (!coder_) {
            throw std::runtime_error("No supported liberasurecode backend found on this system");
        }
    }

    int k = 4;
    int m = 2;
    ec_backend_id_t selected_backend;
    std::unique_ptr<ErasureCoder> coder_;
};

TEST_F(ErasureCoderTest, EncodeDecodeSuccess) {
    size_t data_size = 1024 * 1024; // 1MB
    std::vector<std::byte> original_data(data_size);
    
    // Fill with random data
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    for (size_t i = 0; i < data_size; ++i) {
        original_data[i] = static_cast<std::byte>(dis(gen));
    }

    // 1. Encode
    auto fragments = coder_->encode(original_data);
    ASSERT_NE(fragments, nullptr);
    ASSERT_GT(fragments->fragment_len, 0);

    // 2. Prepare fragments for decoding (simulate all available)
    std::vector<std::vector<std::byte>> available_fragments(k + m);
    for (int i = 0; i < k; ++i) {
        available_fragments[i].assign(
            reinterpret_cast<std::byte*>(fragments->data[i]),
            reinterpret_cast<std::byte*>(fragments->data[i]) + fragments->fragment_len);
    }
    for (int i = 0; i < m; ++i) {
        available_fragments[k + i].assign(
            reinterpret_cast<std::byte*>(fragments->parity[i]),
            reinterpret_cast<std::byte*>(fragments->parity[i]) + fragments->fragment_len);
    }

    // 3. Decode
    auto decoded = coder_->decode(available_fragments);
    ASSERT_NE(decoded, nullptr);
    ASSERT_EQ(decoded->data_len, data_size);

    // 4. Verify
    auto decoded_span = decoded->span();
    ASSERT_TRUE(std::equal(original_data.begin(), original_data.end(), 
                          reinterpret_cast<const std::byte*>(decoded->data)));
}

TEST_F(ErasureCoderTest, RecoveryWithMissingDataFragments) {
    size_t data_size = 1024 * 1024;
    std::vector<std::byte> original_data(data_size, std::byte{0xAA});

    auto fragments = coder_->encode(original_data);
    
    // Simulate missing fragments (drop first 2 data fragments)
    std::vector<std::vector<std::byte>> available_fragments(k + m);
    
    // Data fragments 2 and 3 are present
    for (int i = 2; i < k; ++i) {
        available_fragments[i].assign(
            reinterpret_cast<std::byte*>(fragments->data[i]),
            reinterpret_cast<std::byte*>(fragments->data[i]) + fragments->fragment_len);
    }
    
    // All parity fragments are present
    for (int i = 0; i < m; ++i) {
        available_fragments[k + i].assign(
            reinterpret_cast<std::byte*>(fragments->parity[i]),
            reinterpret_cast<std::byte*>(fragments->parity[i]) + fragments->fragment_len);
    }

    // Attempt decode
    auto decoded = coder_->decode(available_fragments);
    ASSERT_NE(decoded, nullptr);
    ASSERT_EQ(decoded->data_len, data_size);
    ASSERT_TRUE(std::equal(original_data.begin(), original_data.end(), 
                          reinterpret_cast<const std::byte*>(decoded->data)));
}

TEST_F(ErasureCoderTest, FailsWithInsufficientFragments) {
    size_t data_size = 1024;
    std::vector<std::byte> original_data(data_size, std::byte{0xBB});

    auto fragments = coder_->encode(original_data);
    
    // Only 3 fragments available (need 4 for k=4)
    std::vector<std::vector<std::byte>> available_fragments(k + m);
    for (int i = 0; i < 3; ++i) {
        available_fragments[i].assign(
            reinterpret_cast<std::byte*>(fragments->data[i]),
            reinterpret_cast<std::byte*>(fragments->data[i]) + fragments->fragment_len);
    }

    // Should throw LiberasurecodeError
    EXPECT_THROW(coder_->decode(available_fragments), LiberasurecodeError);
}
